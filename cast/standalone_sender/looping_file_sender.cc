// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/looping_file_sender.h"

#include <utility>

#if defined(CAST_STANDALONE_SENDER_HAVE_LIBAOM)
#include "cast/standalone_sender/streaming_av1_encoder.h"
#endif
#include "cast/standalone_sender/streaming_vpx_encoder.h"
#include "platform/base/trivial_clock_traits.h"
#include "util/osp_logging.h"
#include "util/trace_logging.h"

namespace openscreen::cast {

LoopingFileSender::LoopingFileSender(Environment& environment,
                                     ConnectionSettings settings,
                                     const SenderSession* session,
                                     SenderSession::ConfiguredSenders senders,
                                     ShutdownCallback shutdown_callback)
    : env_(environment),
      settings_(std::move(settings)),
      session_(session),
      shutdown_callback_(std::move(shutdown_callback)),
      audio_encoder_(senders.audio_sender->config().channels,
                     StreamingOpusEncoder::kDefaultCastAudioFramesPerSecond,
                     std::move(senders.audio_sender)),
      video_encoder_(CreateVideoEncoder(
          StreamingVideoEncoder::Parameters{.codec = settings.codec},
          env_.task_runner(),
          std::move(senders.video_sender))),
      next_task_(env_.now_function(), env_.task_runner()),
      console_update_task_(env_.now_function(), env_.task_runner()) {
  // Opus and Vp8 are the default values for the config, and if these are set
  // to a different value that means we offered a codec that we do not
  // support, which is a developer error.
  OSP_CHECK(senders.audio_config.codec == AudioCodec::kOpus);
  OSP_CHECK(senders.video_config.codec == VideoCodec::kVp8 ||
            senders.video_config.codec == VideoCodec::kVp9 ||
            senders.video_config.codec == VideoCodec::kAv1);
  OSP_LOG_INFO << "Max allowed media bitrate (audio + video) will be "
               << settings_.max_bitrate;
  bandwidth_being_utilized_ = settings_.max_bitrate / 2;
  UpdateEncoderBitrates();

  next_task_.Schedule([this] { SendFileAgain(); }, Alarm::kImmediately);
}

LoopingFileSender::~LoopingFileSender() = default;

void LoopingFileSender::SetPlaybackRate(double rate) {
  video_capturer_->SetPlaybackRate(rate);
  audio_capturer_->SetPlaybackRate(rate);
}

void LoopingFileSender::UpdateEncoderBitrates() {
  if (bandwidth_being_utilized_ >= kHighBandwidthThreshold) {
    audio_encoder_.UseHighQuality();
  } else {
    audio_encoder_.UseStandardQuality();
  }
  video_encoder_->SetTargetBitrate(bandwidth_being_utilized_ -
                                   audio_encoder_.GetBitrate());
}

void LoopingFileSender::ControlForNetworkCongestion() {
  bandwidth_estimate_ = session_->GetEstimatedNetworkBandwidth();
  if (bandwidth_estimate_ > 0) {
    // Don't ever try to use *all* of the network bandwidth! However, don't go
    // below the absolute minimum requirement either.
    constexpr double kGoodNetworkCitizenFactor = 0.8;
    const int usable_bandwidth = std::max<int>(
        kGoodNetworkCitizenFactor * bandwidth_estimate_, kMinRequiredBitrate);

    // See "congestion control" discussion in the class header comments for
    // BandwidthEstimator.
    if (usable_bandwidth > bandwidth_being_utilized_) {
      constexpr double kConservativeIncrease = 1.1;
      bandwidth_being_utilized_ = std::min<int>(
          bandwidth_being_utilized_ * kConservativeIncrease, usable_bandwidth);
    } else {
      bandwidth_being_utilized_ = usable_bandwidth;
    }

    // Repsect the user's maximum bitrate setting.
    bandwidth_being_utilized_ =
        std::min(bandwidth_being_utilized_, settings_.max_bitrate);

    UpdateEncoderBitrates();
  } else {
    // There is no current bandwidth estimate. So, nothing should be adjusted.
  }

  next_task_.ScheduleFromNow([this] { ControlForNetworkCongestion(); },
                             kCongestionCheckInterval);
}

void LoopingFileSender::SendFileAgain() {
  OSP_LOG_INFO << "Sending " << settings_.path_to_file
               << " (starts in one second)...";
  TRACE_DEFAULT_SCOPED(TraceCategory::kStandaloneSender);

  OSP_CHECK_EQ(num_capturers_running_, 0);
  num_capturers_running_ = 2;
  capture_begin_time_ = latest_frame_time_ = env_.now() + seconds(1);
  audio_capturer_.emplace(
      env_, settings_.path_to_file.c_str(), audio_encoder_.num_channels(),
      audio_encoder_.sample_rate(), capture_begin_time_, *this);
  video_capturer_.emplace(env_, settings_.path_to_file.c_str(),
                          capture_begin_time_, *this);

  next_task_.ScheduleFromNow([this] { ControlForNetworkCongestion(); },
                             kCongestionCheckInterval);
  console_update_task_.Schedule([this] { UpdateStatusOnConsole(); },
                                capture_begin_time_);
}

void LoopingFileSender::OnAudioData(const float* interleaved_samples,
                                    int num_samples,
                                    Clock::time_point capture_begin_time,
                                    Clock::time_point capture_end_time,
                                    Clock::time_point reference_time) {
  TRACE_SCOPED2(TraceCategory::kStandaloneSender, "OnAudioData", "num_samples",
                std::to_string(num_samples), "reference_time",
                ToString(reference_time));
  latest_frame_time_ = std::max(reference_time, latest_frame_time_);
  audio_encoder_.EncodeAndSend(interleaved_samples, num_samples,
                               capture_begin_time, capture_end_time,
                               reference_time);
}

void LoopingFileSender::OnVideoFrame(const AVFrame& av_frame,
                                     Clock::time_point capture_begin_time,
                                     Clock::time_point capture_end_time,
                                     Clock::time_point reference_time) {
  TRACE_SCOPED1(TraceCategory::kStandaloneSender, "OnVideoFrame",
                "reference_time", ToString(reference_time));
  latest_frame_time_ = std::max(reference_time, latest_frame_time_);

  const int src_w = av_frame.width - av_frame.crop_left - av_frame.crop_right;
  const int src_h = av_frame.height - av_frame.crop_top - av_frame.crop_bottom;
  const uint8_t* src_y = av_frame.data[0] + av_frame.crop_left +
                          av_frame.linesize[0] * av_frame.crop_top;
  const uint8_t* src_u = av_frame.data[1] + av_frame.crop_left / 2 +
                          av_frame.linesize[1] * av_frame.crop_top / 2;
  const uint8_t* src_v = av_frame.data[2] + av_frame.crop_left / 2 +
                          av_frame.linesize[2] * av_frame.crop_top / 2;

  StreamingVideoEncoder::VideoFrame frame{};
  frame.capture_begin_time = capture_begin_time;
  frame.capture_end_time = capture_end_time;

  if (src_w == kDisplayWidth && src_h == kDisplayHeight) {
    // No padding needed -- pass through directly.
    frame.width = src_w;
    frame.height = src_h;
    frame.yuv_planes[0] = src_y;
    frame.yuv_planes[1] = src_u;
    frame.yuv_planes[2] = src_v;
    for (int i = 0; i < 3; ++i)
      frame.yuv_strides[i] = av_frame.linesize[i];
  } else {
    // Pad/letterbox to kDisplayWidth x kDisplayHeight.
    if (!padded_initialized_) {
      padded_y_.assign(kDisplayWidth * kDisplayHeight, 16);   // black Y
      padded_u_.assign(kDisplayWidth / 2 * kDisplayHeight / 2, 128);
      padded_v_.assign(kDisplayWidth / 2 * kDisplayHeight / 2, 128);
      padded_initialized_ = true;
    }

    // Scale to fit while preserving aspect ratio.
    int dst_w, dst_h;
    if (src_w * kDisplayHeight > src_h * kDisplayWidth) {
      // Wider than display -- letterbox (bars top/bottom)
      dst_w = kDisplayWidth;
      dst_h = src_h * kDisplayWidth / src_w;
    } else {
      // Taller than display -- pillarbox (bars left/right)
      dst_h = kDisplayHeight;
      dst_w = src_w * kDisplayHeight / src_h;
    }
    dst_w &= ~1;
    dst_h &= ~1;

    const int x_off = (kDisplayWidth - dst_w) / 2;
    const int y_off = (kDisplayHeight - dst_h) / 2;

    // Clear the padded frame to black.
    std::memset(padded_y_.data(), 16, padded_y_.size());
    std::memset(padded_u_.data(), 128, padded_u_.size());
    std::memset(padded_v_.data(), 128, padded_v_.size());

    // Copy source into center of padded frame (no scaling, just center).
    // If src matches dst dimensions, this is a direct copy.
    // For simplicity, we don't scale -- we center-crop/pad.
    const int copy_w = std::min(src_w, dst_w);
    const int copy_h = std::min(src_h, dst_h);
    const int src_x = (src_w - copy_w) / 2;
    const int src_y_off = (src_h - copy_h) / 2;

    // Y plane
    for (int row = 0; row < copy_h; ++row) {
      std::memcpy(
          padded_y_.data() + (y_off + row) * kDisplayWidth + x_off,
          src_y + (src_y_off + row) * av_frame.linesize[0] + src_x,
          copy_w);
    }
    // U plane
    for (int row = 0; row < copy_h / 2; ++row) {
      std::memcpy(
          padded_u_.data() + (y_off / 2 + row) * (kDisplayWidth / 2) + x_off / 2,
          src_u + (src_y_off / 2 + row) * av_frame.linesize[1] + src_x / 2,
          copy_w / 2);
    }
    // V plane
    for (int row = 0; row < copy_h / 2; ++row) {
      std::memcpy(
          padded_v_.data() + (y_off / 2 + row) * (kDisplayWidth / 2) + x_off / 2,
          src_v + (src_y_off / 2 + row) * av_frame.linesize[2] + src_x / 2,
          copy_w / 2);
    }

    frame.width = kDisplayWidth;
    frame.height = kDisplayHeight;
    frame.yuv_planes[0] = padded_y_.data();
    frame.yuv_planes[1] = padded_u_.data();
    frame.yuv_planes[2] = padded_v_.data();
    frame.yuv_strides[0] = kDisplayWidth;
    frame.yuv_strides[1] = kDisplayWidth / 2;
    frame.yuv_strides[2] = kDisplayWidth / 2;
  }

  video_encoder_->EncodeAndSend(frame, reference_time, {});
}

void LoopingFileSender::UpdateStatusOnConsole() {
  const Clock::duration elapsed = latest_frame_time_ - capture_begin_time_;
  const auto seconds_part = to_seconds(elapsed);
  const auto millis_part = to_milliseconds(elapsed - seconds_part);
  // The control codes here attempt to erase the current line the cursor is
  // on, and then print out the updated status text. If the terminal does not
  // support simple ANSI escape codes, the following will still work, but
  // there might sometimes be old status lines not getting erased (i.e., just
  // partially overwritten).
  fprintf(stdout,
          "\r\x1b[2K\rLoopingFileSender: At %01" PRId64
          ".%03ds in file (est. network bandwidth: %d kbps). \n",
          static_cast<int64_t>(seconds_part.count()),
          static_cast<int>(millis_part.count()), bandwidth_estimate_ / 1024);
  fflush(stdout);

  console_update_task_.ScheduleFromNow([this] { UpdateStatusOnConsole(); },
                                       kConsoleUpdateInterval);
}

void LoopingFileSender::OnEndOfFile(SimulatedCapturer* capturer) {
  OSP_LOG_INFO << "The " << ToTrackName(capturer)
               << " capturer has reached the end of the media stream.";
  --num_capturers_running_;
  if (num_capturers_running_ == 0) {
    console_update_task_.Cancel();

    if (settings_.should_loop_video) {
      OSP_DLOG_INFO << "Starting the media stream over again.";
      next_task_.Schedule([this] { SendFileAgain(); }, Alarm::kImmediately);
    } else {
      OSP_DLOG_INFO << "Video complete. Exiting...";
      shutdown_callback_();
    }
  }
}

void LoopingFileSender::OnError(SimulatedCapturer* capturer,
                                const std::string& message) {
  OSP_LOG_ERROR << "The " << ToTrackName(capturer)
                << " has failed: " << message;
  --num_capturers_running_;
  // If both fail, the application just pauses. This accounts for things like
  // "file not found" errors. However, if only one track fails, then keep
  // going.
}

const char* LoopingFileSender::ToTrackName(SimulatedCapturer* capturer) const {
  if (capturer == &*audio_capturer_) {
    return "audio";
  } else if (capturer == &*video_capturer_) {
    return "video";
  } else {
    OSP_NOTREACHED();
  }
}

std::unique_ptr<StreamingVideoEncoder> LoopingFileSender::CreateVideoEncoder(
    const StreamingVideoEncoder::Parameters& params,
    TaskRunner& task_runner,
    std::unique_ptr<Sender> sender) {
  switch (params.codec) {
    case VideoCodec::kVp8:
    case VideoCodec::kVp9:
      return std::make_unique<StreamingVpxEncoder>(params, task_runner,
                                                   std::move(sender));
    case VideoCodec::kAv1:
#if defined(CAST_STANDALONE_SENDER_HAVE_LIBAOM)
      return std::make_unique<StreamingAv1Encoder>(params, task_runner,
                                                   std::move(sender));
#else
      OSP_LOG_FATAL << "AV1 codec selected, but could not be used because "
                       "LibAOM not installed.";
      return nullptr;
#endif
    default:
      // Since we only support VP8, VP9, and AV1, any other codec value here
      // should be due only to developer error.
      OSP_LOG_ERROR << "Unsupported codec " << CodecToString(params.codec);
      OSP_NOTREACHED();
  }
}

}  // namespace openscreen::cast
