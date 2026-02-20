// X11 screen/window capturer implementation.
//
// Runs capture + BGRA-to-I420 conversion on a dedicated thread to avoid
// blocking the OpenScreen task runner. Delivers finished frames by posting
// to the task runner.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

// X11 macros that conflict with OpenScreen C++ code.
#undef None
#undef Status
#undef Bool
#undef True
#undef False

#include "cast/standalone_sender/x11_capturer.h"

#include <chrono>

#include "cast/streaming/public/environment.h"
#include "platform/api/task_runner.h"
#include "util/osp_logging.h"

namespace openscreen::cast {

struct X11Capturer::X11State {
  Display* display = nullptr;
  bool use_shm = false;
  XShmSegmentInfo shm_info{};
  XImage* shm_image = nullptr;
};

X11Capturer::X11Capturer(Environment& env, int fps, FrameCallback callback)
    : env_(env), fps_(fps), callback_(std::move(callback)) {
  x11_ = std::make_unique<X11State>();
  x11_->display = XOpenDisplay(nullptr);
  if (!x11_->display) {
    OSP_LOG_FATAL << "Cannot open X11 display";
    return;
  }
  target_window_ = DefaultRootWindow(x11_->display);

  XWindowAttributes attrs;
  XGetWindowAttributes(x11_->display, target_window_, &attrs);
  width_ = attrs.width & ~1;
  height_ = attrs.height & ~1;

  // Close this display -- the thread will open its own connection
  XCloseDisplay(x11_->display);
  x11_->display = nullptr;

  OSP_LOG_INFO << "X11Capturer: " << width_ << "x" << height_
               << " @ " << fps_ << " fps (desktop)";
  thread_ = std::thread(&X11Capturer::CaptureThread, this);
}

X11Capturer::X11Capturer(Environment& env, int fps, unsigned long window_id,
                         FrameCallback callback)
    : env_(env), fps_(fps), callback_(std::move(callback)),
      capture_window_(true), target_window_(window_id) {
  x11_ = std::make_unique<X11State>();
  x11_->display = XOpenDisplay(nullptr);
  if (!x11_->display) {
    OSP_LOG_FATAL << "Cannot open X11 display";
    return;
  }

  XWindowAttributes attrs;
  XGetWindowAttributes(x11_->display, target_window_, &attrs);
  width_ = attrs.width & ~1;
  height_ = attrs.height & ~1;

  XCloseDisplay(x11_->display);
  x11_->display = nullptr;

  OSP_LOG_INFO << "X11Capturer: " << width_ << "x" << height_
               << " @ " << fps_ << " fps (window)";
  thread_ = std::thread(&X11Capturer::CaptureThread, this);
}

X11Capturer::~X11Capturer() {
  running_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
}

void X11Capturer::CaptureThread() {
  // Each thread needs its own X11 display connection
  Display* display = XOpenDisplay(nullptr);
  if (!display) {
    OSP_LOG_ERROR << "X11Capturer thread: cannot open display";
    return;
  }

  Window window = capture_window_ ? target_window_
                                   : DefaultRootWindow(display);
  const int w = width_;
  const int h = height_;

  // Set up XShm
  bool use_shm = XShmQueryExtension(display);
  XShmSegmentInfo shm_info{};
  XImage* shm_image = nullptr;

  if (use_shm) {
    shm_image = XShmCreateImage(
        display,
        DefaultVisual(display, DefaultScreen(display)),
        DefaultDepth(display, DefaultScreen(display)),
        ZPixmap, nullptr, &shm_info, w, h);
    if (shm_image) {
      shm_info.shmid = shmget(IPC_PRIVATE,
                               shm_image->bytes_per_line * shm_image->height,
                               IPC_CREAT | 0777);
      if (shm_info.shmid >= 0) {
        shm_info.shmaddr = shm_image->data =
            static_cast<char*>(shmat(shm_info.shmid, nullptr, 0));
        shm_info.readOnly = 0;
        XShmAttach(display, &shm_info);
        shmctl(shm_info.shmid, IPC_RMID, nullptr);
      } else {
        XDestroyImage(shm_image);
        shm_image = nullptr;
        use_shm = false;
      }
    } else {
      use_shm = false;
    }
  }

  OSP_LOG_INFO << "X11Capturer thread: started"
               << (use_shm ? " (XShm)" : " (XGetImage)");

  // Allocate I420 buffers (thread-local, no sharing needed)
  std::vector<uint8_t> y_buf(w * h);
  std::vector<uint8_t> u_buf((w / 2) * (h / 2));
  std::vector<uint8_t> v_buf((w / 2) * (h / 2));

  const auto frame_duration = std::chrono::microseconds(1000000 / fps_);

  while (running_) {
    auto t0 = std::chrono::steady_clock::now();
    auto capture_begin = env_.now();

    // Capture
    XImage* image = nullptr;
    if (use_shm && shm_image) {
      XShmGetImage(display, window, shm_image, 0, 0, AllPlanes);
      image = shm_image;
    } else {
      image = XGetImage(display, window, 0, 0, w, h, AllPlanes, ZPixmap);
    }

    if (!image) {
      std::this_thread::sleep_for(frame_duration);
      continue;
    }

    // Convert BGRA -> I420
    const auto* bgra = reinterpret_cast<const uint8_t*>(image->data);
    const int stride = image->bytes_per_line;

    for (int y = 0; y < h; y += 2) {
      const uint8_t* row0 = bgra + y * stride;
      const uint8_t* row1 = bgra + (y + 1) * stride;
      uint8_t* y0 = y_buf.data() + y * w;
      uint8_t* y1 = y_buf.data() + (y + 1) * w;
      uint8_t* u_row = u_buf.data() + (y / 2) * (w / 2);
      uint8_t* v_row = v_buf.data() + (y / 2) * (w / 2);

      for (int x = 0; x < w; x += 2) {
        int b00 = row0[x*4], g00 = row0[x*4+1], r00 = row0[x*4+2];
        int b10 = row0[(x+1)*4], g10 = row0[(x+1)*4+1], r10 = row0[(x+1)*4+2];
        int b01 = row1[x*4], g01 = row1[x*4+1], r01 = row1[x*4+2];
        int b11 = row1[(x+1)*4], g11 = row1[(x+1)*4+1], r11 = row1[(x+1)*4+2];

        y0[x]     = ((66*r00 + 129*g00 + 25*b00 + 128) >> 8) + 16;
        y0[x + 1] = ((66*r10 + 129*g10 + 25*b10 + 128) >> 8) + 16;
        y1[x]     = ((66*r01 + 129*g01 + 25*b01 + 128) >> 8) + 16;
        y1[x + 1] = ((66*r11 + 129*g11 + 25*b11 + 128) >> 8) + 16;

        int ar = (r00+r10+r01+r11) >> 2;
        int ag = (g00+g10+g01+g11) >> 2;
        int ab = (b00+b10+b01+b11) >> 2;
        u_row[x/2] = ((-38*ar - 74*ag + 112*ab + 128) >> 8) + 128;
        v_row[x/2] = ((112*ar - 94*ag - 18*ab + 128) >> 8) + 128;
      }
    }

    if (!use_shm) {
      XDestroyImage(image);
    }

    auto capture_end = env_.now();

    // Copy frame data for the task runner (the buffers here will be
    // overwritten on next capture)
    auto y_copy = std::make_shared<std::vector<uint8_t>>(y_buf);
    auto u_copy = std::make_shared<std::vector<uint8_t>>(u_buf);
    auto v_copy = std::make_shared<std::vector<uint8_t>>(v_buf);

    env_.task_runner().PostTask(
        [this, y_copy, u_copy, v_copy, w, h,
         capture_begin, capture_end] {
          Frame frame;
          frame.width = w;
          frame.height = h;
          frame.y = y_copy->data();
          frame.u = u_copy->data();
          frame.v = v_copy->data();
          frame.y_stride = w;
          frame.u_stride = w / 2;
          frame.v_stride = w / 2;
          frame.capture_begin = capture_begin;
          frame.capture_end = capture_end;
          callback_(frame, capture_begin);
        });

    // Sleep for the remainder of the frame interval
    auto elapsed = std::chrono::steady_clock::now() - t0;
    if (elapsed < frame_duration) {
      std::this_thread::sleep_for(frame_duration - elapsed);
    }
  }

  // Cleanup
  if (use_shm && shm_image) {
    XShmDetach(display, &shm_info);
    shmdt(shm_info.shmaddr);
    XDestroyImage(shm_image);
  }
  XCloseDisplay(display);
  OSP_LOG_INFO << "X11Capturer thread: stopped";
}

}  // namespace openscreen::cast
