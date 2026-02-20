// X11 screen sender - wires X11/PulseAudio capture to Cast streaming encoders.
// Replaces LoopingFileSender for live screen mirroring.

#ifndef CAST_STANDALONE_SENDER_X11_SENDER_H_
#define CAST_STANDALONE_SENDER_X11_SENDER_H_

#include <memory>
#include <optional>

#include "cast/standalone_sender/connection_settings.h"
#include "cast/standalone_sender/pulse_capturer.h"
#include "cast/standalone_sender/streaming_opus_encoder.h"
#include "cast/standalone_sender/streaming_video_encoder.h"
#include "cast/standalone_sender/x11_capturer.h"
#include "cast/streaming/public/sender_session.h"

namespace openscreen::cast {

class Environment;

// Captures the X11 screen and system audio, encodes them, and streams
// via the Cast protocol.
class X11Sender {
 public:
  using ShutdownCallback = std::function<void()>;

  X11Sender(Environment& environment,
            ConnectionSettings settings,
            const SenderSession* session,
            SenderSession::ConfiguredSenders senders,
            ShutdownCallback shutdown_callback);

  ~X11Sender();

 private:
  void OnVideoFrame(const X11Capturer::Frame& frame,
                    Clock::time_point reference_time);
  void OnAudioData(const float* interleaved_samples,
                   int num_samples,
                   Clock::time_point capture_begin,
                   Clock::time_point capture_end,
                   Clock::time_point reference_time);
  void ControlForNetworkCongestion();
  void UpdateEncoderBitrates();
  void UpdateStatusOnConsole();

  std::unique_ptr<StreamingVideoEncoder> CreateVideoEncoder(
      const StreamingVideoEncoder::Parameters& params,
      TaskRunner& task_runner,
      std::unique_ptr<Sender> sender);

  Environment& env_;
  const ConnectionSettings settings_;
  const SenderSession* session_;
  ShutdownCallback shutdown_callback_;

  int bandwidth_estimate_ = 0;
  int bandwidth_being_utilized_;

  StreamingOpusEncoder audio_encoder_;
  std::unique_ptr<StreamingVideoEncoder> video_encoder_;

  std::unique_ptr<X11Capturer> video_capturer_;
  std::unique_ptr<PulseCapturer> audio_capturer_;

  Alarm congestion_alarm_;
  Alarm console_alarm_;
  Clock::time_point start_time_{};

};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_X11_SENDER_H_
