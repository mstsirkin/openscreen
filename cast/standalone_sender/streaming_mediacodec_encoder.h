// Hardware video encoder for Android using AMediaCodec NDK API.
// Replaces software VP8/VP9 encoding with hardware H.264 encoding,
// enabling 1080p@30fps on mobile devices.

#ifndef CAST_STANDALONE_SENDER_STREAMING_MEDIACODEC_ENCODER_H_
#define CAST_STANDALONE_SENDER_STREAMING_MEDIACODEC_ENCODER_H_

#ifdef __ANDROID__

#include <media/NdkMediaCodec.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
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

 private:
  void OutputThread();
  bool ConfigureEncoder(int width, int height);
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
  std::queue<FrameMeta> pending_meta_;

  // SPS/PPS codec config data, prepended to each key frame.
  std::vector<uint8_t> codec_config_;

  // Shared flag for weak reference in posted tasks.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace openscreen::cast

#endif  // __ANDROID__

#endif  // CAST_STANDALONE_SENDER_STREAMING_MEDIACODEC_ENCODER_H_
