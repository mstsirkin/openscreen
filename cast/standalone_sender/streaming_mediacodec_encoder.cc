// Hardware video encoder for Android using AMediaCodec NDK API.

#ifdef __ANDROID__

#include "cast/standalone_sender/streaming_mediacodec_encoder.h"

#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/eglext.h>
#include <android/log.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "cast/streaming/public/encoded_frame.h"
#include "util/osp_logging.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "MediaCodecEnc", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MediaCodecEnc", __VA_ARGS__)

namespace openscreen::cast {

namespace {

constexpr char kVertexShader[] = R"(
attribute vec2 aPosition;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main() {
  gl_Position = vec4(aPosition, 0.0, 1.0);
  vTexCoord = aTexCoord;
}
)";

constexpr char kFragmentShader[] = R"(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D uYTex;
uniform sampler2D uUTex;
uniform sampler2D uVTex;
void main() {
  float y = texture2D(uYTex, vTexCoord).r;
  float u = texture2D(uUTex, vTexCoord).r - 0.5;
  float v = texture2D(uVTex, vTexCoord).r - 0.5;
  float r = y + 1.402 * v;
  float g = y - 0.344136 * u - 0.714136 * v;
  float b = y + 1.772 * u;
  gl_FragColor = vec4(r, g, b, 1.0);
}
)";

GLuint CompileShader(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled != GL_TRUE) {
    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(log_length, '\0');
    if (log_length > 0) {
      glGetShaderInfoLog(shader, log_length, nullptr, log.data());
    }
    LOGE("Shader compile failed: %s", log.c_str());
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

GLuint LinkProgram(GLuint vertex_shader, GLuint fragment_shader) {
  GLuint program = glCreateProgram();
  glAttachShader(program, vertex_shader);
  glAttachShader(program, fragment_shader);
  glLinkProgram(program);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(log_length, '\0');
    if (log_length > 0) {
      glGetProgramInfoLog(program, log_length, nullptr, log.data());
    }
    LOGE("Program link failed: %s", log.c_str());
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

void FillTexCoordsForRotation(int rotation_degrees, GLfloat* texcoords) {
  switch (rotation_degrees) {
    case 90:
      texcoords[0] = 1.f; texcoords[1] = 1.f;
      texcoords[2] = 1.f; texcoords[3] = 0.f;
      texcoords[4] = 0.f; texcoords[5] = 1.f;
      texcoords[6] = 0.f; texcoords[7] = 0.f;
      return;
    case 180:
      texcoords[0] = 1.f; texcoords[1] = 0.f;
      texcoords[2] = 0.f; texcoords[3] = 0.f;
      texcoords[4] = 1.f; texcoords[5] = 1.f;
      texcoords[6] = 0.f; texcoords[7] = 1.f;
      return;
    case 270:
      texcoords[0] = 0.f; texcoords[1] = 0.f;
      texcoords[2] = 0.f; texcoords[3] = 1.f;
      texcoords[4] = 1.f; texcoords[5] = 0.f;
      texcoords[6] = 1.f; texcoords[7] = 1.f;
      return;
    default:
      texcoords[0] = 0.f; texcoords[1] = 1.f;
      texcoords[2] = 1.f; texcoords[3] = 1.f;
      texcoords[4] = 0.f; texcoords[5] = 0.f;
      texcoords[6] = 1.f; texcoords[7] = 0.f;
      return;
  }
}

}  // namespace

StreamingMediaCodecEncoder::StreamingMediaCodecEncoder(
    const Parameters& params,
    TaskRunner& task_runner,
    std::unique_ptr<Sender> sender)
    : StreamingVideoEncoder(params, task_runner, std::move(sender)) {
  LOGI("Created (hardware H.264)");
}

StreamingMediaCodecEncoder::~StreamingMediaCodecEncoder() {
  if (alive_) {
    *alive_ = false;
    alive_.reset();
  }
  running_ = false;
  if (output_thread_.joinable()) {
    output_thread_.join();
  }
  DestroyGlResources();
  if (codec_) {
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
  }
}

std::unique_ptr<Sender> StreamingMediaCodecEncoder::ReleaseSender() {
  if (alive_) {
    *alive_ = false;
    alive_.reset();
  }
  running_ = false;
  if (output_thread_.joinable()) {
    output_thread_.join();
  }
  DestroyGlResources();
  if (codec_) {
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
  }
  pending_meta_.clear();
  configured_width_ = 0;
  configured_height_ = 0;
  use_surface_input_ = false;
  start_time_ = Clock::time_point::min();
  last_enqueued_rtp_timestamp_ = RtpTimeTicks();
  last_output_rtp_timestamp_ = RtpTimeTicks();
  return TakeSender();
}

int StreamingMediaCodecEncoder::GetTargetBitrate() const {
  return target_bitrate_.load();
}

void StreamingMediaCodecEncoder::SetTargetBitrate(int new_bitrate) {
  target_bitrate_.store(new_bitrate);
  // Request a bitrate change via MediaCodec parameter update.
  if (codec_) {
    AMediaFormat* params = AMediaFormat_new();
    AMediaFormat_setInt32(params, AMEDIAFORMAT_KEY_BIT_RATE, new_bitrate);
    // REQUEST_SYNC_FRAME triggers a key frame which also applies
    // the new bitrate.
    AMediaFormat_setInt32(params, "request-sync", 0);
    AMediaCodec_setParameters(codec_, params);
    AMediaFormat_delete(params);
  }
}

bool StreamingMediaCodecEncoder::ConfigureEncoder(int width,
                                                  int height,
                                                  bool use_surface_input) {
  DestroyGlResources();
  if (codec_) {
    running_ = false;
    if (output_thread_.joinable()) {
      output_thread_.join();
    }
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
  }

  codec_ = AMediaCodec_createEncoderByType("video/avc");
  if (!codec_) {
    LOGE("Failed to create H.264 encoder");
    return false;
  }

  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE,
                        target_bitrate_.load());
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, 30);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 5);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                        use_surface_input ? 0x7F000789 : 19);

  media_status_t status = AMediaCodec_configure(
      codec_, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
  AMediaFormat_delete(format);

  if (status != AMEDIA_OK && !use_surface_input) {
    LOGE("Configure failed: %d. Trying YUV420SemiPlanar...", status);
    // Some devices prefer NV12 (SemiPlanar = 21)
    format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE,
                          target_bitrate_.load());
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, 30);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 5);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21);
    status = AMediaCodec_configure(codec_, format, nullptr, nullptr,
                                   AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);
    if (status != AMEDIA_OK) {
      LOGE("Configure failed with both formats: %d", status);
      AMediaCodec_delete(codec_);
      codec_ = nullptr;
      return false;
    }
  } else if (status != AMEDIA_OK) {
    LOGE("Configure failed for surface input: %d", status);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
    return false;
  }

  if (use_surface_input) {
    media_status_t surface_status =
        AMediaCodec_createInputSurface(codec_, &input_window_);
    if (surface_status != AMEDIA_OK || !input_window_) {
      LOGE("Failed to create input surface: %d", surface_status);
      AMediaCodec_delete(codec_);
      codec_ = nullptr;
      return false;
    }
  }

  status = AMediaCodec_start(codec_);
  if (status != AMEDIA_OK) {
    LOGE("Start failed: %d", status);
    if (input_window_) {
      ANativeWindow_release(input_window_);
      input_window_ = nullptr;
    }
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
    return false;
  }

  configured_width_ = width;
  configured_height_ = height;
  use_surface_input_ = use_surface_input;
  running_ = true;
  output_thread_ = std::thread(&StreamingMediaCodecEncoder::OutputThread, this);

  if (use_surface_input_ && !EnsureGlResources(width, height)) {
    running_ = false;
    if (output_thread_.joinable()) {
      output_thread_.join();
    }
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
    return false;
  }

  LOGI("Configured %dx%d H.264 hardware encoder (%s input)", width, height,
       use_surface_input_ ? "surface" : "byte-buffer");
  return true;
}

void StreamingMediaCodecEncoder::EncodeAndSend(
    const VideoFrame& frame,
    Clock::time_point reference_time,
    std::function<void(Stats)> stats_callback) {
  const bool needs_surface_input = frame.rotation_degrees != 0;
  if (frame.width != configured_width_ || frame.height != configured_height_ ||
      needs_surface_input != use_surface_input_) {
    if (!ConfigureEncoder(frame.width, frame.height, needs_surface_input)) {
      OSP_LOG_ERROR << "Failed to configure MediaCodec encoder for "
                    << frame.width << "x" << frame.height;
      return;
    }
  }

  if (!codec_) return;

  // Compute RTP timestamp.
  RtpTimeTicks rtp_timestamp;
  if (start_time_ == Clock::time_point::min()) {
    const auto frame_step = RtpTimeDelta::FromDuration(
        frame.duration > Clock::duration::zero() ? frame.duration
                                                 : std::chrono::milliseconds(33),
        sender_->rtp_timebase());
    if (sender_->GetNextFrameId() == FrameId::first()) {
      start_time_ = reference_time;
      rtp_timestamp = RtpTimeTicks();
      last_enqueued_rtp_timestamp_ = RtpTimeTicks();
    } else {
      last_enqueued_rtp_timestamp_ = sender_->GetLastEnqueuedRtpTimestamp();
      rtp_timestamp = last_enqueued_rtp_timestamp_ + frame_step;
      start_time_ = reference_time -
          rtp_timestamp.ToTimeSinceOrigin<Clock::duration>(
              sender_->rtp_timebase());
    }
  } else {
    rtp_timestamp = RtpTimeTicks::FromTimeSinceOrigin(
        reference_time - start_time_, sender_->rtp_timebase());
    if (rtp_timestamp <= last_enqueued_rtp_timestamp_) {
      return;  // Drop: timestamp not increasing.
    }
  }

  // Check in-flight duration. If too high (e.g. after pause/play),
  // re-sync the timeline and request a key frame so recovery is quick.
  if (sender_->GetInFlightMediaDuration(rtp_timestamp) >
      sender_->GetMaxInFlightMediaDuration()) {
    start_time_ = reference_time -
        last_enqueued_rtp_timestamp_.ToTimeSinceOrigin<Clock::duration>(
            sender_->rtp_timebase());
    needs_key_frame_ = true;
    return;
  }

  if (use_surface_input_) {
    if (!EncodeAndSendViaSurface(frame, reference_time, rtp_timestamp)) {
      OSP_LOG_ERROR << "Failed to encode rotated frame via surface input";
    }
    last_enqueued_rtp_timestamp_ = rtp_timestamp;
    return;
  }

  // Get an input buffer.
  ssize_t buf_idx = AMediaCodec_dequeueInputBuffer(codec_, 0);
  if (buf_idx < 0) {
    return;  // No input buffer available, drop frame.
  }

  size_t buf_size = 0;
  uint8_t* buf = AMediaCodec_getInputBuffer(codec_, buf_idx, &buf_size);
  if (!buf) {
    return;
  }

  // Copy I420 YUV data into the input buffer.
  const int y_size = frame.width * frame.height;
  const int uv_size = (frame.width / 2) * (frame.height / 2);
  const size_t needed = y_size + uv_size * 2;
  if (buf_size < needed) {
    AMediaCodec_queueInputBuffer(codec_, buf_idx, 0, 0, 0, 0);
    return;
  }

  // Copy Y plane.
  for (int row = 0; row < frame.height; ++row) {
    std::memcpy(buf + row * frame.width,
                frame.yuv_planes[0] + row * frame.yuv_strides[0],
                frame.width);
  }
  // Copy U plane.
  uint8_t* u_dst = buf + y_size;
  for (int row = 0; row < frame.height / 2; ++row) {
    std::memcpy(u_dst + row * (frame.width / 2),
                frame.yuv_planes[1] + row * frame.yuv_strides[1],
                frame.width / 2);
  }
  // Copy V plane.
  uint8_t* v_dst = u_dst + uv_size;
  for (int row = 0; row < frame.height / 2; ++row) {
    std::memcpy(v_dst + row * (frame.width / 2),
                frame.yuv_planes[2] + row * frame.yuv_strides[2],
                frame.width / 2);
  }

  // Request key frame if needed.
  uint32_t flags = 0;
  if (needs_key_frame_.exchange(false)) {
    AMediaFormat* params = AMediaFormat_new();
    AMediaFormat_setInt32(params, "request-sync", 0);
    AMediaCodec_setParameters(codec_, params);
    AMediaFormat_delete(params);
  }

  auto pts = std::chrono::duration_cast<std::chrono::microseconds>(
                 reference_time.time_since_epoch())
                 .count();
  {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    pending_meta_.push_back({pts, reference_time, rtp_timestamp});
  }
  AMediaCodec_queueInputBuffer(codec_, buf_idx, 0, needed, pts, flags);

  last_enqueued_rtp_timestamp_ = rtp_timestamp;
}

bool StreamingMediaCodecEncoder::EnsureGlResources(int width, int height) {
  if (!input_window_) {
    LOGE("No input window for surface mode");
    return false;
  }

  egl_display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (egl_display_ == EGL_NO_DISPLAY) {
    LOGE("eglGetDisplay failed");
    return false;
  }
  if (!eglInitialize(egl_display_, nullptr, nullptr)) {
    LOGE("eglInitialize failed");
    return false;
  }

  const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_RECORDABLE_ANDROID, 1,
      EGL_NONE,
  };
  EGLint num_configs = 0;
  if (!eglChooseConfig(egl_display_, config_attribs, &egl_config_, 1,
                       &num_configs) ||
      num_configs != 1) {
    LOGE("eglChooseConfig failed");
    return false;
  }

  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  egl_context_ =
      eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT,
                       context_attribs);
  if (egl_context_ == EGL_NO_CONTEXT) {
    LOGE("eglCreateContext failed");
    return false;
  }

  egl_surface_ =
      eglCreateWindowSurface(egl_display_, egl_config_, input_window_, nullptr);
  if (egl_surface_ == EGL_NO_SURFACE) {
    LOGE("eglCreateWindowSurface failed");
    return false;
  }

  if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)) {
    LOGE("eglMakeCurrent failed");
    return false;
  }
  eglSwapInterval(egl_display_, 0);

  const GLuint vertex_shader = CompileShader(GL_VERTEX_SHADER, kVertexShader);
  const GLuint fragment_shader =
      CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
  if (!vertex_shader || !fragment_shader) {
    return false;
  }

  gl_program_ = LinkProgram(vertex_shader, fragment_shader);
  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);
  if (!gl_program_) {
    return false;
  }

  glUseProgram(gl_program_);
  gl_position_location_ = glGetAttribLocation(gl_program_, "aPosition");
  gl_texcoord_location_ = glGetAttribLocation(gl_program_, "aTexCoord");
  gl_y_sampler_location_ = glGetUniformLocation(gl_program_, "uYTex");
  gl_u_sampler_location_ = glGetUniformLocation(gl_program_, "uUTex");
  gl_v_sampler_location_ = glGetUniformLocation(gl_program_, "uVTex");

  glGenTextures(3, gl_textures_);
  for (int i = 0; i < 3; ++i) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, gl_textures_[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glUniform1i(gl_y_sampler_location_, 0);
  glUniform1i(gl_u_sampler_location_, 1);
  glUniform1i(gl_v_sampler_location_, 2);
  glViewport(0, 0, width, height);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  return glGetError() == GL_NO_ERROR;
}

void StreamingMediaCodecEncoder::DestroyGlResources() {
  if (egl_display_ != EGL_NO_DISPLAY) {
    eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
  }
  if (gl_textures_[0] || gl_textures_[1] || gl_textures_[2]) {
    glDeleteTextures(3, gl_textures_);
    gl_textures_[0] = gl_textures_[1] = gl_textures_[2] = 0;
  }
  if (gl_program_) {
    glDeleteProgram(gl_program_);
    gl_program_ = 0;
  }
  if (egl_surface_ != EGL_NO_SURFACE) {
    eglDestroySurface(egl_display_, egl_surface_);
    egl_surface_ = EGL_NO_SURFACE;
  }
  if (egl_context_ != EGL_NO_CONTEXT) {
    eglDestroyContext(egl_display_, egl_context_);
    egl_context_ = EGL_NO_CONTEXT;
  }
  if (egl_display_ != EGL_NO_DISPLAY) {
    eglTerminate(egl_display_);
    egl_display_ = EGL_NO_DISPLAY;
  }
  if (input_window_) {
    ANativeWindow_release(input_window_);
    input_window_ = nullptr;
  }
  gl_luma_width_ = gl_luma_height_ = 0;
  gl_chroma_width_ = gl_chroma_height_ = 0;
}

void StreamingMediaCodecEncoder::UpdateRotationGeometry(int frame_width,
                                                        int frame_height,
                                                        int rotation_degrees) {
  const float output_aspect =
      static_cast<float>(configured_width_) / configured_height_;
  float content_aspect =
      static_cast<float>(frame_width) / frame_height;
  if (rotation_degrees == 90 || rotation_degrees == 270) {
    content_aspect = 1.0f / content_aspect;
  }

  float quad_width = 1.0f;
  float quad_height = 1.0f;
  if (content_aspect > output_aspect) {
    quad_height = output_aspect / content_aspect;
  } else {
    quad_width = content_aspect / output_aspect;
  }

  quad_positions_[0] = -quad_width; quad_positions_[1] = -quad_height;
  quad_positions_[2] = quad_width;  quad_positions_[3] = -quad_height;
  quad_positions_[4] = -quad_width; quad_positions_[5] = quad_height;
  quad_positions_[6] = quad_width;  quad_positions_[7] = quad_height;
  FillTexCoordsForRotation(rotation_degrees, quad_texcoords_);
}

bool StreamingMediaCodecEncoder::EncodeAndSendViaSurface(
    const VideoFrame& frame,
    Clock::time_point reference_time,
    RtpTimeTicks rtp_timestamp) {
  if (!codec_ || egl_display_ == EGL_NO_DISPLAY || !gl_program_) {
    return false;
  }

  if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)) {
    LOGE("eglMakeCurrent failed for frame");
    return false;
  }

  if (needs_key_frame_.exchange(false)) {
    AMediaFormat* params = AMediaFormat_new();
    AMediaFormat_setInt32(params, "request-sync", 0);
    AMediaCodec_setParameters(codec_, params);
    AMediaFormat_delete(params);
  }

  UpdateRotationGeometry(frame.width, frame.height, frame.rotation_degrees);

  const int chroma_width = frame.width / 2;
  const int chroma_height = frame.height / 2;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glUseProgram(gl_program_);

  const uint8_t* planes[] = {
      frame.yuv_planes[0], frame.yuv_planes[1], frame.yuv_planes[2]};
  const int widths[] = {frame.width, chroma_width, chroma_width};
  const int heights[] = {frame.height, chroma_height, chroma_height};
  const int strides[] = {
      frame.yuv_strides[0], frame.yuv_strides[1], frame.yuv_strides[2]};

  for (int i = 0; i < 3; ++i) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, gl_textures_[i]);
    if ((i == 0 && (gl_luma_width_ != widths[i] || gl_luma_height_ != heights[i])) ||
        (i != 0 &&
         (gl_chroma_width_ != widths[i] || gl_chroma_height_ != heights[i]))) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, widths[i], heights[i], 0,
                   GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
    }
    std::vector<uint8_t> packed;
    const uint8_t* upload = planes[i];
    if (strides[i] != widths[i]) {
      packed.resize(widths[i] * heights[i]);
      for (int row = 0; row < heights[i]; ++row) {
        std::memcpy(packed.data() + row * widths[i],
                    planes[i] + row * strides[i], widths[i]);
      }
      upload = packed.data();
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, widths[i], heights[i],
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, upload);
  }
  gl_luma_width_ = frame.width;
  gl_luma_height_ = frame.height;
  gl_chroma_width_ = chroma_width;
  gl_chroma_height_ = chroma_height;

  glClear(GL_COLOR_BUFFER_BIT);
  glVertexAttribPointer(gl_position_location_, 2, GL_FLOAT, GL_FALSE, 0,
                        quad_positions_);
  glVertexAttribPointer(gl_texcoord_location_, 2, GL_FLOAT, GL_FALSE, 0,
                        quad_texcoords_);
  glEnableVertexAttribArray(gl_position_location_);
  glEnableVertexAttribArray(gl_texcoord_location_);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  const auto pts = std::chrono::duration_cast<std::chrono::microseconds>(
                       reference_time.time_since_epoch())
                       .count();
  static const auto presentation_time_android =
      reinterpret_cast<PFNEGLPRESENTATIONTIMEANDROIDPROC>(
          eglGetProcAddress("eglPresentationTimeANDROID"));
  if (presentation_time_android) {
    presentation_time_android(egl_display_, egl_surface_, pts * 1000);
  }
  {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    pending_meta_.push_back({pts, reference_time, rtp_timestamp});
  }
  if (!eglSwapBuffers(egl_display_, egl_surface_)) {
    LOGE("eglSwapBuffers failed");
    std::lock_guard<std::mutex> lock(meta_mutex_);
    if (!pending_meta_.empty()) {
      pending_meta_.pop_front();
    }
    return false;
  }
  return true;
}

void StreamingMediaCodecEncoder::OutputThread() {
  LOGI("Output thread started");
  while (running_) {
    AMediaCodecBufferInfo info;
    ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 10000);
    if (idx < 0) {
      continue;
    }

    // Save codec config (SPS/PPS) to prepend to key frames.
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
      size_t out_size = 0;
      uint8_t* out_buf = AMediaCodec_getOutputBuffer(codec_, idx, &out_size);
      if (out_buf && info.size > 0) {
        codec_config_.assign(out_buf + info.offset,
                             out_buf + info.offset + info.size);
        LOGI("Saved codec config: %d bytes", info.size);
      }
      AMediaCodec_releaseOutputBuffer(codec_, idx, false);
      continue;
    }

    if (info.size > 0) {
      size_t out_size = 0;
      uint8_t* out_buf = AMediaCodec_getOutputBuffer(codec_, idx, &out_size);
      if (out_buf && info.size > 0) {
        // Key frame flag: value 1 is sync frame (not defined in older NDKs).
        bool is_key = (info.flags & 1) != 0;

        // Retrieve the original reference_time and rtp_timestamp
        // queued at input time, so A/V sync is preserved despite
        // the encoder's internal buffering latency.
        Clock::time_point ref_time;
        RtpTimeTicks rtp_ts;
        {
          std::lock_guard<std::mutex> lock(meta_mutex_);
          if (!pending_meta_.empty()) {
            auto& meta = pending_meta_.front();
            ref_time = meta.reference_time;
            rtp_ts = meta.rtp_timestamp;
            pending_meta_.pop_front();
          } else {
            // Fallback: reconstruct from PTS.
            ref_time = Clock::time_point(
                std::chrono::duration_cast<Clock::duration>(
                    std::chrono::microseconds(info.presentationTimeUs)));
            rtp_ts = RtpTimeTicks::FromTimeSinceOrigin(
                ref_time - start_time_, sender_->rtp_timebase());
          }
        }

        // Prepend SPS/PPS to key frames so the receiver can decode.
        std::vector<uint8_t> data;
        if (is_key && !codec_config_.empty()) {
          data.reserve(codec_config_.size() + info.size);
          data.insert(data.end(), codec_config_.begin(), codec_config_.end());
          data.insert(data.end(), out_buf + info.offset,
                      out_buf + info.offset + info.size);
        } else {
          data.assign(out_buf + info.offset,
                      out_buf + info.offset + info.size);
        }

        std::weak_ptr<bool> weak_alive = alive_;
        main_task_runner_.PostTask(
            [this, weak_alive, data = std::move(data), is_key, ref_time,
             rtp_ts]() mutable {
              if (weak_alive.expired()) return;
              SendEncodedFrame(std::move(data), is_key, ref_time,
                               ref_time, ref_time,
                               std::chrono::milliseconds(33), rtp_ts);
            });
      }
    }

    AMediaCodec_releaseOutputBuffer(codec_, idx, false);
  }
  LOGI("Output thread stopped");
}

void StreamingMediaCodecEncoder::SendEncodedFrame(
    std::vector<uint8_t> data,
    bool is_key_frame,
    Clock::time_point reference_time,
    Clock::time_point capture_begin_time,
    Clock::time_point capture_end_time,
    Clock::duration duration,
    RtpTimeTicks rtp_timestamp) {
  EncodedFrame frame;
  frame.frame_id = sender_->GetNextFrameId();
  // First frame must be a key frame.
  if (frame.frame_id == FrameId::first()) {
    is_key_frame = true;
  }
  if (is_key_frame) {
    frame.dependency = EncodedFrame::Dependency::kKeyFrame;
    frame.referenced_frame_id = frame.frame_id;
  } else {
    frame.dependency = EncodedFrame::Dependency::kDependent;
    frame.referenced_frame_id = frame.frame_id - 1;
  }
  frame.rtp_timestamp = rtp_timestamp;
  frame.reference_time = reference_time;
  frame.new_playout_delay = {};
  frame.data = data;
  if (sender_->EnqueueFrame(frame) != Sender::OK) {
    needs_key_frame_ = true;
  }
}

}  // namespace openscreen::cast

#endif  // __ANDROID__
