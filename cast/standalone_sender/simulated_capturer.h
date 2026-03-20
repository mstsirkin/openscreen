// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_SIMULATED_CAPTURER_H_
#define CAST_STANDALONE_SENDER_SIMULATED_CAPTURER_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "cast/standalone_sender/ffmpeg_glue.h"
#include "platform/api/time.h"
#include "platform/base/span.h"
#include "util/alarm.h"

namespace openscreen::cast {

class Environment;

// Simulates live media capture by demuxing, decoding, and emitting a stream of
// frames from a file at normal (1X) speed. This is a base class containing
// common functionality. Typical usage: Instantiate one SimulatedAudioCapturer
// and one FileVideoStreamCapturer.
class SimulatedCapturer {
 public:
  // Interface for receiving end-of-stream and fatal error notifications.
  class Observer {
   public:
    // Called once the end of the file has been reached and the `capturer` has
    // halted.
    virtual void OnEndOfFile(SimulatedCapturer* capturer) = 0;

    // Called if a non-recoverable error occurs and the `capturer` has halted.
    virtual void OnError(SimulatedCapturer* capturer,
                         const std::string& message) = 0;

   protected:
    virtual ~Observer();
  };

  void SetPlaybackRate(double rate);
  void SeekTo(Clock::duration media_time, Clock::time_point new_start_time);
  // Seek and decode one frame immediately, even when paused.
  void SeekAndDeliverOneFrame(Clock::duration media_time,
                              Clock::time_point reference_time);
  int display_rotation_degrees() const { return display_rotation_degrees_; }

 protected:
  SimulatedCapturer(Environment& environment,
                    const char* path,
                    AVMediaType media_type,
                    Clock::time_point start_time,
                    Clock::duration start_media_time,
                    Observer& observer);

  virtual ~SimulatedCapturer();

  // Optionally overridden, to apply additional decoder context settings before
  // avcodec_open2() is called.
  virtual void SetAdditionalDecoderParameters(AVCodecContext* decoder_context);

  // Performs any additional processing on the decoded frame (e.g., audio
  // resampling), and returns any adjustments to the frame's capture time (e.g.,
  // to account for any buffering). If a fatal error occurs, std::nullopt is
  // returned. The default implementation does nothing.
  //
  // Mutating the `decoded_frame` is not allowed. If a subclass implementation
  // wants to deliver different data (e.g., resampled audio), it must stash the
  // data itself for the next DeliverDataToClient() call.
  virtual std::optional<Clock::duration> ProcessDecodedFrame(
      const AVFrame& decoded_frame);

  // Delivers the decoded frame data to the client.
  virtual void DeliverDataToClient(const AVFrame& decoded_frame,
                                   Clock::time_point capture_begin_time,
                                   Clock::time_point capture_end_time,
                                   Clock::time_point reference_time) = 0;

  // Called when any transient or fatal error occurs, generating an Error and
  // scheduling a task to notify the Observer of it soon.
  void OnError(const char* what, int av_errnum);

  // Converts the given FFMPEG tick count into an approximate Clock::duration.
  static Clock::duration ToApproximateClockDuration(
      int64_t ticks,
      const AVRational& time_base);

 private:
  // Reads the next frame from the file, sends it to the decoder, and schedules
  // a future ConsumeNextDecodedFrame() call to continue processing.
  void StartDecodingNextFrame();

  // Receives the next decoded frame and schedules media delivery to the client,
  // and/or calls Observer::OnEndOfFile() if there are no more frames in the
  // file.
  void ConsumeNextDecodedFrame();

  const AVFormatContextUniquePtr format_context_;
  ClockNowFunctionPtr now_;
  const AVMediaType media_type_;  // Audio or Video.
  Clock::time_point start_time_;
  Clock::duration start_media_time_;
  Observer& observer_;
  const AVPacketUniquePtr packet_;        // Decoder input buffer.
  const AVFrameUniquePtr decoded_frame_;  // Decoder output frame.
  int stream_index_ = -1;                 // Selected stream from the file.
  AVCodecContextUniquePtr decoder_context_;

  // When the current frame capture was started.
  Clock::time_point capture_begin_time_;

  // The last frame's stream timestamp. This is used to detect bad stream
  // timestamps in the file.
  std::optional<Clock::duration> last_frame_timestamp_;

  // Used to schedule the next task to execute and when it should execute. There
  // is only ever one task scheduled/running at any time.
  Alarm next_task_;

  // Used to determine playback rate. Currently, we only support "playing"
  // at 1x speed, or "pausing" at 0x speed.
  bool playback_rate_is_non_zero_ = true;
  int display_rotation_degrees_ = 0;
};

// Emits the primary audio stream from a file.
class SimulatedAudioCapturer final : public SimulatedCapturer {
 public:
  class Client : public SimulatedCapturer::Observer {
   public:
    // Called to deliver more audio data as `interleaved_samples`, which
    // contains `num_samples` tuples (i.e., multiply by the number of channels
    // to determine the number of array elements). `reference_time` is used to
    // synchronize the play-out of the first audio sample with respect to video
    // frames.
    virtual void OnAudioData(const float* interleaved_samples,
                             int num_samples,
                             Clock::time_point capture_begin_time,
                             Clock::time_point capture_end_time,
                             Clock::time_point reference_time) = 0;

   protected:
    ~Client() override;
  };

  // Constructor: `num_channels` and `sample_rate` specify the required audio
  // format. If necessary, audio from the file will be resampled to match the
  // required format.
  SimulatedAudioCapturer(Environment& environment,
                         const char* path,
                         int num_channels,
                         int sample_rate,
                         Clock::time_point start_time,
                         Clock::duration start_media_time,
                         Client& client);

  ~SimulatedAudioCapturer() final;

 private:
  // Examines the audio format of the given `frame`, and ensures the resampler
  // is initialized to take that as input.
  bool EnsureResamplerIsInitializedFor(const AVFrame& frame);

  // Resamples the current `SimulatedCapturer::decoded_frame()` into the
  // required output format/channels/rate. The result is stored in
  // `resampled_audio_` for the next DeliverDataToClient() call.
  std::optional<Clock::duration> ProcessDecodedFrame(
      const AVFrame& decoded_frame) final;

  // Called at the moment Client::OnAudioData() should be called to pass the
  // `resampled_audio_`.
  void DeliverDataToClient(const AVFrame& decoded_frame,
                           Clock::time_point capture_begin_time,
                           Clock::time_point capture_end_time,
                           Clock::time_point reference_time) final;

  const int num_channels_;  // Output number of channels.
  const int sample_rate_;   // Output sample rate.
  Client& client_;

  const SwrContextUniquePtr resampler_;

  // Current resampler input audio parameters.
  AVSampleFormat input_sample_format_ = AV_SAMPLE_FMT_NONE;
  int input_sample_rate_;
  // Opaque value used by resampler library.
#if _LIBAVUTIL_OLD_CHANNEL_LAYOUT
  uint64_t input_channel_layout_;
#else
  AVChannelLayout input_channel_layout_;
#endif  // _LIBAVUTIL_OLD_CHANNEL_LAYOUT

  std::vector<float> resampled_audio_;
};

// Emits the primary video stream from a file.
class SimulatedVideoCapturer final : public SimulatedCapturer {
 public:
  class Client : public SimulatedCapturer::Observer {
   public:
    // Called to deliver the next video `frame`, which is always in I420 format.
    // `reference_time` is used to synchronize the play-out of the video frame
    // with respect to the audio track.
    virtual void OnVideoFrame(const AVFrame& frame,
                              Clock::time_point capture_begin_time,
                              Clock::time_point capture_end_time,
                              Clock::time_point reference_time) = 0;

   protected:
    ~Client() override;
  };

  SimulatedVideoCapturer(Environment& environment,
                         const char* path,
                         Clock::time_point start_time,
                         Clock::duration start_media_time,
                         Client& client);

  ~SimulatedVideoCapturer() final;

 private:
  Client& client_;

  // Sets up the decoder to produce I420 format output.
  void SetAdditionalDecoderParameters(AVCodecContext* decoder_context) final;

  // Called at the moment Client::OnVideoFrame() should be called to provide the
  // next video frame.
  void DeliverDataToClient(const AVFrame& decoded_frame,
                           Clock::time_point capture_begin_time,
                           Clock::time_point capture_end_time,
                           Clock::time_point reference_time) final;
};

struct VideoPassthroughInfo {
  bool eligible = false;
  std::string reason;
  int width = 0;
  int height = 0;
  int display_rotation_degrees = 0;
};

// Emits compressed H.264 video packets from an MP4-like container for direct
// streaming, without decode/re-encode.
class SimulatedVideoPassthroughCapturer final {
 public:
  class Client {
   public:
    virtual void OnEndOfFile(SimulatedVideoPassthroughCapturer* capturer) = 0;
    virtual void OnError(SimulatedVideoPassthroughCapturer* capturer,
                         const std::string& message) = 0;
    virtual void OnVideoPacket(ByteView data,
                               bool is_key_frame,
                               Clock::duration media_timestamp,
                               Clock::duration media_duration,
                               Clock::time_point capture_begin_time,
                               Clock::time_point capture_end_time,
                               Clock::time_point reference_time) = 0;

   protected:
    virtual ~Client();
  };

  static VideoPassthroughInfo Probe(const char* path);

  SimulatedVideoPassthroughCapturer(Environment& environment,
                                    const char* path,
                                    Clock::time_point start_time,
                                    Clock::duration start_media_time,
                                    Client& client);
  ~SimulatedVideoPassthroughCapturer();

  void SetPlaybackRate(double rate);
  void SeekTo(Clock::duration media_time, Clock::time_point new_start_time);
  void Continue();

 private:
  void StartReadingNextPacket();
  void DeliverCurrentPacket();
  void OnError(const char* what, int av_errnum);
  bool InitializeAvcConfiguration();
  bool ConvertPacketToAnnexB(const AVPacket& packet, bool prepend_parameter_sets);
  static Clock::duration ToApproximateClockDuration(int64_t ticks,
                                                    const AVRational& time_base);

  const AVFormatContextUniquePtr format_context_;
  ClockNowFunctionPtr now_;
  Clock::time_point start_time_;
  Clock::duration start_media_time_;
  Client& client_;
  const AVPacketUniquePtr packet_;
  int stream_index_ = -1;
  Alarm next_task_;
  bool playback_rate_is_non_zero_ = true;
  std::optional<Clock::duration> last_packet_timestamp_;
  Clock::time_point capture_begin_time_;
  int nal_length_size_ = 4;
  std::vector<uint8_t> parameter_sets_annexb_;
  std::vector<uint8_t> filtered_packet_storage_;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_SIMULATED_CAPTURER_H_
