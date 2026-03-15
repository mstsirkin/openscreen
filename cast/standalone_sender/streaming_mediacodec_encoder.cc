// Hardware video encoder for Android using AMediaCodec NDK API.

#ifdef __ANDROID__

#include "cast/standalone_sender/streaming_mediacodec_encoder.h"

#include <android/log.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <cstring>

#include "cast/streaming/public/encoded_frame.h"
#include "util/osp_logging.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "MediaCodecEnc", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MediaCodecEnc", __VA_ARGS__)

namespace openscreen::cast {

StreamingMediaCodecEncoder::StreamingMediaCodecEncoder(
    const Parameters& params,
    TaskRunner& task_runner,
    std::unique_ptr<Sender> sender)
    : StreamingVideoEncoder(params, task_runner, std::move(sender)) {
  LOGI("Created (hardware H.264)");
}

StreamingMediaCodecEncoder::~StreamingMediaCodecEncoder() {
  running_ = false;
  if (output_thread_.joinable()) {
    output_thread_.join();
  }
  if (codec_) {
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
  }
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

bool StreamingMediaCodecEncoder::ConfigureEncoder(int width, int height) {
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
                        19);  // YUV420Planar = 19

  media_status_t status = AMediaCodec_configure(
      codec_, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
  AMediaFormat_delete(format);

  if (status != AMEDIA_OK) {
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
  }

  status = AMediaCodec_start(codec_);
  if (status != AMEDIA_OK) {
    LOGE("Start failed: %d", status);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
    return false;
  }

  configured_width_ = width;
  configured_height_ = height;
  running_ = true;
  output_thread_ = std::thread(&StreamingMediaCodecEncoder::OutputThread, this);

  LOGI("Configured %dx%d H.264 hardware encoder", width, height);
  return true;
}

void StreamingMediaCodecEncoder::EncodeAndSend(
    const VideoFrame& frame,
    Clock::time_point reference_time,
    std::function<void(Stats)> stats_callback) {
  if (frame.width != configured_width_ || frame.height != configured_height_) {
    if (!ConfigureEncoder(frame.width, frame.height)) {
      OSP_LOG_ERROR << "Failed to configure MediaCodec encoder for "
                    << frame.width << "x" << frame.height;
      return;
    }
  }

  if (!codec_) return;

  // Compute RTP timestamp.
  RtpTimeTicks rtp_timestamp;
  if (start_time_ == Clock::time_point::min()) {
    start_time_ = reference_time;
    rtp_timestamp = RtpTimeTicks();
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
    pending_meta_.push({pts, reference_time, rtp_timestamp});
  }
  AMediaCodec_queueInputBuffer(codec_, buf_idx, 0, needed, pts, flags);

  last_enqueued_rtp_timestamp_ = rtp_timestamp;
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
            pending_meta_.pop();
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
