// Hardware video encoder for Android using AMediaCodec NDK API.
// Replaces software VP8/VP9 encoding with hardware H.264 encoding,
// enabling 1080p@30fps on mobile devices.

#ifndef CAST_STANDALONE_SENDER_STREAMING_MEDIACODEC_ENCODER_H_
#define CAST_STANDALONE_SENDER_STREAMING_MEDIACODEC_ENCODER_H_

#ifdef __ANDROID__

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <deque>
#include <thread>

#include "cast/standalone_sender/streaming_video_encoder.h"

namespace openscreen::cast {

class StreamingMediaCodecEncoder : public StreamingVideoEncoder {
 public:
  StreamingMediaCodecEncoder(const Parameters& params,
                             TaskRunner& task_runner,
                             std::unique_ptr<Sender> sender);
  ~StreamingMediaCodecEncoder() override;

  int GetTargetBitrate() const override;
  void SetTargetBitrate(int new_bitrate) override;
  void EncodeAndSend(const VideoFrame& frame,
                     Clock::time_point reference_time,
                     std::function<void(Stats)> stats_callback) override;
  std::unique_ptr<Sender> ReleaseSender() override;

 private:
  void OutputThread();
  bool ConfigureEncoder(int width, int height, bool use_surface_input);
  bool EnsureGlResources(int width, int height);
  void DestroyGlResources();
  bool EncodeAndSendViaSurface(const VideoFrame& frame,
                               Clock::time_point reference_time,
                               RtpTimeTicks rtp_timestamp);
  void UpdateRotationGeometry(int frame_width,
                              int frame_height,
                              int rotation_degrees);
  void SendEncodedFrame(std::vector<uint8_t> data,
                        bool is_key_frame,
                        Clock::time_point reference_time,
                        Clock::time_point capture_begin_time,
                        Clock::time_point capture_end_time,
                        Clock::duration duration,
                        RtpTimeTicks rtp_timestamp);

  AMediaCodec* codec_ = nullptr;
  int configured_width_ = 0;
  int configured_height_ = 0;
  bool use_surface_input_ = false;
  std::atomic<int> target_bitrate_{2 << 20};
  std::atomic<bool> needs_key_frame_{true};
  std::atomic<bool> running_{false};
  std::thread output_thread_;

  Clock::time_point start_time_ = Clock::time_point::min();
  RtpTimeTicks last_enqueued_rtp_timestamp_;

  // Per-frame metadata queued at input, dequeued at output.
  struct FrameMeta {
    int64_t pts_us;
    Clock::time_point reference_time;
    RtpTimeTicks rtp_timestamp;
  };
  std::mutex meta_mutex_;
  std::deque<FrameMeta> pending_meta_;
  RtpTimeTicks last_output_rtp_timestamp_;

  // SPS/PPS codec config data, prepended to each key frame.
  std::vector<uint8_t> codec_config_;

  ANativeWindow* input_window_ = nullptr;
  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLContext egl_context_ = EGL_NO_CONTEXT;
  EGLSurface egl_surface_ = EGL_NO_SURFACE;
  EGLConfig egl_config_ = nullptr;
  GLuint gl_program_ = 0;
  GLuint gl_position_location_ = 0;
  GLuint gl_texcoord_location_ = 0;
  GLint gl_y_sampler_location_ = -1;
  GLint gl_u_sampler_location_ = -1;
  GLint gl_v_sampler_location_ = -1;
  GLuint gl_textures_[3] = {0, 0, 0};
  int gl_luma_width_ = 0;
  int gl_luma_height_ = 0;
  int gl_chroma_width_ = 0;
  int gl_chroma_height_ = 0;
  GLfloat quad_positions_[8] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
  GLfloat quad_texcoords_[8] = {0.f, 1.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f};

  // Shared flag for weak reference in posted tasks.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace openscreen::cast

#endif  // __ANDROID__

#endif  // CAST_STANDALONE_SENDER_STREAMING_MEDIACODEC_ENCODER_H_
