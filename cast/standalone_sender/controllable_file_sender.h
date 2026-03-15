// Copyright 2026

#ifndef CAST_STANDALONE_SENDER_CONTROLLABLE_FILE_SENDER_H_
#define CAST_STANDALONE_SENDER_CONTROLLABLE_FILE_SENDER_H_

#include <functional>
#include <memory>
#include <optional>
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
                                     public SimulatedVideoCapturer::Client {
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

  Clock::duration GetCurrentPosition() const;
  Clock::duration GetDuration() const;
  bool is_playing() const { return is_playing_; }

 private:
#ifdef __ANDROID__
  static constexpr int kDisplayWidth = 854;
  static constexpr int kDisplayHeight = 480;
#else
  static constexpr int kDisplayWidth = 1920;
  static constexpr int kDisplayHeight = 1080;
#endif

  void UpdateEncoderBitrates();
  void ControlForNetworkCongestion();
  void StartPlaybackAt(Clock::duration position);
  void StopCapturers();
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

  void OnEndOfFile(SimulatedCapturer* capturer) final;
  void OnError(SimulatedCapturer* capturer,
               const std::string& message) final;

  std::unique_ptr<StreamingVideoEncoder> CreateVideoEncoder(
      const StreamingVideoEncoder::Parameters& params,
      TaskRunner& task_runner,
      std::unique_ptr<Sender> sender);
  void PrepareBaseVideoFrame(const AVFrame& av_frame,
                             StreamingVideoEncoder::VideoFrame* frame);
  void ApplyViewportTransform(StreamingVideoEncoder::VideoFrame* frame);

  Environment& env_;
  const ConnectionSettings settings_;
  const SenderSession* session_;
  ShutdownCallback shutdown_callback_;

  int bandwidth_estimate_ = 0;
  int bandwidth_being_utilized_;

  StreamingOpusEncoder audio_encoder_;
  std::unique_ptr<StreamingVideoEncoder> video_encoder_;

  std::optional<SimulatedAudioCapturer> audio_capturer_;
  std::optional<SimulatedVideoCapturer> video_capturer_;
  int num_capturers_running_ = 0;

  Alarm next_task_;
  Alarm console_update_task_;

  Clock::duration media_duration_{};
  Clock::duration start_position_{};
  Clock::duration last_known_position_{};
  Clock::time_point playback_start_time_{};
  bool is_playing_ = false;

  VideoViewport viewport_;
  int video_frame_count_ = 0;

  std::vector<uint8_t> padded_y_;
  std::vector<uint8_t> padded_u_;
  std::vector<uint8_t> padded_v_;
  std::vector<uint8_t> transformed_y_;
  std::vector<uint8_t> transformed_u_;
  std::vector<uint8_t> transformed_v_;
  SwsContext* viewport_scaler_ = nullptr;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_CONTROLLABLE_FILE_SENDER_H_
