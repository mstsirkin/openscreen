// Copyright 2026

#ifndef CAST_STANDALONE_SENDER_CONTROLLABLE_FILE_SENDER_H_
#define CAST_STANDALONE_SENDER_CONTROLLABLE_FILE_SENDER_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cast/standalone_sender/connection_settings.h"
#include "cast/standalone_sender/constants.h"
#include "cast/standalone_sender/simulated_capturer.h"
#include "cast/standalone_sender/streaming_opus_encoder.h"
#include "cast/standalone_sender/streaming_video_encoder.h"
#include "cast/streaming/public/sender_session.h"

struct SwsContext;

namespace openscreen::cast {

class Environment;

struct VideoViewport {
  double zoom = 1.0;
  double center_x = 0.5;
  double center_y = 0.5;
};

// Streams a local file via Cast mirroring and exposes controls required by a
// mobile UI: play/pause, seek, and viewport changes.
class ControllableFileSender final : public SimulatedAudioCapturer::Client,
                                     public SimulatedVideoCapturer::Client,
                                     public SimulatedVideoPassthroughCapturer::Client,
                                     public Sender::Observer {
 public:
  using ShutdownCallback = std::function<void()>;

  ControllableFileSender(Environment& environment,
                         ConnectionSettings settings,
                         const SenderSession* session,
                         SenderSession::ConfiguredSenders senders,
                         ShutdownCallback shutdown_callback);
  ~ControllableFileSender() final;

  void Play();
  void Pause();
  void Stop();
  void SeekTo(Clock::duration position);
  void SeekBy(Clock::duration delta);

  void SetViewport(const VideoViewport& viewport);
  void ResetViewport();
  void SetAvSyncOffset(Clock::duration offset);

  Clock::duration GetCurrentPosition() const;
  Clock::duration GetDuration() const;
  bool is_playing() const { return is_playing_; }
  std::string GetActiveModeString() const;

 private:
  static constexpr int kDisplayWidth = 1920;
  static constexpr int kDisplayHeight = 1080;

  void UpdateEncoderBitrates();
  void ControlForNetworkCongestion();
  void StartPlaybackAt(Clock::duration position);
  void StartPausedKeepaliveAt(Clock::duration position);
  void StopCapturers();
  bool CanUseVideoPassthrough();
  bool CanStartVideoPassthroughAt(Clock::duration position) const;
  bool IsViewportIdentity() const;
  void EnsureVideoEncoderCreated();
  void FallbackToTranscode(const char* reason,
                           Clock::duration position,
                           bool resume_playback,
                           bool disable_passthrough = false);
  void SchedulePausedKeepalive();
  void SendPausedKeepaliveFrame();
  void RetryPendingPassthroughPacket();
  void UpdateStatusOnConsole();
  Clock::duration ClampPosition(Clock::duration position) const;
  VideoViewport ClampViewport(const VideoViewport& viewport) const;

  void OnAudioData(const float* interleaved_samples,
                   int num_samples,
                   Clock::time_point capture_begin_time,
                   Clock::time_point capture_end_time,
                   Clock::time_point reference_time) final;
  void OnVideoFrame(const AVFrame& frame,
                    Clock::time_point capture_begin_time,
                    Clock::time_point capture_end_time,
                    Clock::time_point reference_time) final;
  void OnVideoPacket(ByteView data,
                     bool is_key_frame,
                     Clock::duration media_timestamp,
                     Clock::duration media_duration,
                     Clock::time_point capture_begin_time,
                     Clock::time_point capture_end_time,
                     Clock::time_point reference_time) final;

  void OnEndOfFile(SimulatedCapturer* capturer) final;
  void OnEndOfFile(SimulatedVideoPassthroughCapturer* capturer) final;
  void OnError(SimulatedCapturer* capturer,
               const std::string& message) final;
  void OnError(SimulatedVideoPassthroughCapturer* capturer,
               const std::string& message) final;
  void OnFrameCanceled(FrameId frame_id) final;
  void OnPictureLost() final;

  std::unique_ptr<StreamingVideoEncoder> CreateVideoEncoder(
      const StreamingVideoEncoder::Parameters& params,
      TaskRunner& task_runner,
      std::unique_ptr<Sender> sender);
  void PrepareBaseVideoFrame(const AVFrame& av_frame,
                             StreamingVideoEncoder::VideoFrame* frame);
  void ApplyViewportTransform(StreamingVideoEncoder::VideoFrame* frame);
  void RotateI420IntoPadded(int rotation_degrees,
                            int src_w,
                            int src_h,
                            int dst_x,
                            int dst_y);

  Environment& env_;
  ConnectionSettings settings_;
  const SenderSession* session_;
  ShutdownCallback shutdown_callback_;

  int bandwidth_estimate_ = 0;
  int bandwidth_being_utilized_;

  StreamingOpusEncoder audio_encoder_;
  std::unique_ptr<StreamingVideoEncoder> video_encoder_;
  std::unique_ptr<Sender> video_sender_;

  std::optional<SimulatedAudioCapturer> audio_capturer_;
  std::optional<SimulatedVideoCapturer> video_capturer_;
  std::optional<SimulatedVideoPassthroughCapturer> video_passthrough_capturer_;
  int num_capturers_running_ = 0;

  Alarm next_task_;
  Alarm console_update_task_;
  Alarm paused_keepalive_task_;
  Alarm passthrough_retry_task_;

  Clock::duration media_duration_{};
  Clock::duration start_position_{};
  Clock::duration last_known_position_{};
  Clock::time_point playback_start_time_{};
  bool is_playing_ = false;
  bool can_passthrough_video_ = false;
  bool passthrough_active_ = false;
  bool passthrough_backpressured_ = false;
  std::string active_mode_;
  struct PendingPassthroughPacket {
    std::vector<uint8_t> data;
    bool is_key_frame = false;
    Clock::duration media_timestamp{};
    Clock::duration media_duration{};
    Clock::time_point capture_begin_time{};
    Clock::time_point capture_end_time{};
    Clock::time_point reference_time{};
  };
  std::optional<PendingPassthroughPacket> pending_passthrough_packet_;

  VideoViewport viewport_;
#if defined(__ANDROID__) && !defined(CAST_STANDALONE_SENDER_HAVE_MEDIACODEC)
  int video_frame_count_ = 0;
#endif

  std::vector<uint8_t> padded_y_;
  std::vector<uint8_t> padded_u_;
  std::vector<uint8_t> padded_v_;
  std::vector<uint8_t> scaled_y_;
  std::vector<uint8_t> scaled_u_;
  std::vector<uint8_t> scaled_v_;
  std::vector<uint8_t> transformed_y_;
  std::vector<uint8_t> transformed_u_;
  std::vector<uint8_t> transformed_v_;
  SwsContext* viewport_scaler_ = nullptr;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_CONTROLLABLE_FILE_SENDER_H_
