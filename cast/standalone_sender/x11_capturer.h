// X11 screen/window capturer for x11cast.
// Captures X11 display content as I420 YUV frames at a target frame rate.
//
// X11 headers are NOT included here to avoid macro conflicts with OpenScreen
// (X11 defines None, Status, etc.). All X11 types are hidden behind an
// opaque implementation struct.

#ifndef CAST_STANDALONE_SENDER_X11_CAPTURER_H_
#define CAST_STANDALONE_SENDER_X11_CAPTURER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "platform/api/time.h"

namespace openscreen {
class TaskRunner;
}

namespace openscreen::cast {

class Environment;

// Captures X11 screen content on a dedicated thread and delivers I420 YUV
// frames to a callback on the task runner thread.
class X11Capturer {
 public:
  struct Frame {
    int width = 0;
    int height = 0;
    const uint8_t* y = nullptr;
    const uint8_t* u = nullptr;
    const uint8_t* v = nullptr;
    int y_stride = 0;
    int u_stride = 0;
    int v_stride = 0;
    Clock::time_point capture_begin;
    Clock::time_point capture_end;
  };

  using FrameCallback = std::function<void(const Frame&, Clock::time_point)>;

  // Capture the entire root window.
  X11Capturer(Environment& env, int fps, FrameCallback callback);

  // Capture a specific window by X11 window ID (unsigned long).
  X11Capturer(Environment& env, int fps, unsigned long window_id,
              FrameCallback callback);

  ~X11Capturer();

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  struct X11State;  // opaque, defined in .cc

  void CaptureThread();

  Environment& env_;
  int fps_;
  FrameCallback callback_;
  bool capture_window_ = false;
  unsigned long target_window_ = 0;

  int width_ = 0;
  int height_ = 0;

  std::unique_ptr<X11State> x11_;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_X11_CAPTURER_H_
