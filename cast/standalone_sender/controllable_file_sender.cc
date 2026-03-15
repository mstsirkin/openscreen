// Copyright 2026

#include "cast/standalone_sender/controllable_file_sender.h"

#include <cinttypes>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#if defined(CAST_STANDALONE_SENDER_HAVE_LIBAOM)
#include "cast/standalone_sender/streaming_av1_encoder.h"
#endif
#if defined(__ANDROID__) && defined(CAST_STANDALONE_SENDER_HAVE_MEDIACODEC)
#include "cast/standalone_sender/streaming_mediacodec_encoder.h"
#endif
#include "cast/standalone_sender/ffmpeg_glue.h"
#include "cast/standalone_sender/streaming_vpx_encoder.h"
#include "platform/base/trivial_clock_traits.h"
#include "util/chrono_helpers.h"
#include "util/osp_logging.h"
#include "util/trace_logging.h"

namespace openscreen::cast {

namespace {

int RoundToEven(int value) {
  return value & ~1;
}

}  // namespace

ControllableFileSender::ControllableFileSender(
    Environment& environment,
    ConnectionSettings settings,
    const SenderSession* session,
    SenderSession::ConfiguredSenders senders,
    ShutdownCallback shutdown_callback)
    : env_(environment),
      settings_(std::move(settings)),
      session_(session),
      shutdown_callback_(std::move(shutdown_callback)),
      bandwidth_being_utilized_(settings_.max_bitrate / 2),
      audio_encoder_(senders.audio_sender->config().channels,
                     StreamingOpusEncoder::kDefaultCastAudioFramesPerSecond,
                     std::move(senders.audio_sender)),
      video_encoder_(CreateVideoEncoder(
          StreamingVideoEncoder::Parameters{.codec = settings_.codec},
          env_.task_runner(),
          std::move(senders.video_sender))),
      next_task_(env_.now_function(), env_.task_runner()),
      console_update_task_(env_.now_function(), env_.task_runner()),
      media_duration_(GetMediaDuration(settings_.path_to_file.c_str())) {
  OSP_CHECK(senders.audio_config.codec == AudioCodec::kOpus);
  OSP_CHECK(senders.video_config.codec == VideoCodec::kVp8 ||
            senders.video_config.codec == VideoCodec::kVp9 ||
            senders.video_config.codec == VideoCodec::kAv1 ||
            senders.video_config.codec == VideoCodec::kH264);

  padded_y_.assign(kDisplayWidth * kDisplayHeight, 16);
  padded_u_.assign(kDisplayWidth / 2 * kDisplayHeight / 2, 128);
  padded_v_.assign(kDisplayWidth / 2 * kDisplayHeight / 2, 128);
  transformed_y_.resize(kDisplayWidth * kDisplayHeight);
  transformed_u_.resize(kDisplayWidth / 2 * kDisplayHeight / 2);
  transformed_v_.resize(kDisplayWidth / 2 * kDisplayHeight / 2);

  UpdateEncoderBitrates();
  Play();
}

ControllableFileSender::~ControllableFileSender() {
  StopCapturers();
  sws_freeContext(viewport_scaler_);
}

void ControllableFileSender::Play() {
  if (is_playing_) {
    return;
  }
  StartPlaybackAt(last_known_position_);
}

void ControllableFileSender::Pause() {
  if (!is_playing_) {
    return;
  }
  last_known_position_ = GetCurrentPosition();
  StopCapturers();
}

void ControllableFileSender::Stop() {
  Pause();
  last_known_position_ = Clock::duration::zero();
}

void ControllableFileSender::SeekTo(Clock::duration position) {
  last_known_position_ = ClampPosition(position);
  if (is_playing_) {
    StartPlaybackAt(last_known_position_);
  }
}

void ControllableFileSender::SeekBy(Clock::duration delta) {
  SeekTo(GetCurrentPosition() + delta);
}

void ControllableFileSender::SetViewport(const VideoViewport& viewport) {
  viewport_ = ClampViewport(viewport);
}

void ControllableFileSender::ResetViewport() {
  viewport_ = VideoViewport{};
}

Clock::duration ControllableFileSender::GetCurrentPosition() const {
  if (!is_playing_) {
    return last_known_position_;
  }
  return ClampPosition(start_position_ + std::max(env_.now() - playback_start_time_,
                                                  Clock::duration::zero()));
}

Clock::duration ControllableFileSender::GetDuration() const {
  return media_duration_;
}

void ControllableFileSender::UpdateEncoderBitrates() {
  if (bandwidth_being_utilized_ >= kHighBandwidthThreshold) {
    audio_encoder_.UseHighQuality();
  } else {
    audio_encoder_.UseStandardQuality();
  }
  video_encoder_->SetTargetBitrate(bandwidth_being_utilized_ -
                                   audio_encoder_.GetBitrate());
}

void ControllableFileSender::ControlForNetworkCongestion() {
  bandwidth_estimate_ = session_->GetEstimatedNetworkBandwidth();
  if (bandwidth_estimate_ > 0) {
    constexpr double kGoodNetworkCitizenFactor = 0.8;
    const int usable_bandwidth = std::max<int>(
        kGoodNetworkCitizenFactor * bandwidth_estimate_, kMinRequiredBitrate);

    if (usable_bandwidth > bandwidth_being_utilized_) {
      constexpr double kConservativeIncrease = 1.1;
      bandwidth_being_utilized_ = std::min<int>(
          bandwidth_being_utilized_ * kConservativeIncrease, usable_bandwidth);
    } else {
      bandwidth_being_utilized_ = usable_bandwidth;
    }

    bandwidth_being_utilized_ =
        std::min(bandwidth_being_utilized_, settings_.max_bitrate);
    UpdateEncoderBitrates();
  }

  next_task_.ScheduleFromNow([this] { ControlForNetworkCongestion(); },
                             kCongestionCheckInterval);
}

void ControllableFileSender::StartPlaybackAt(Clock::duration position) {
  StopCapturers();
  start_position_ = ClampPosition(position);
  last_known_position_ = start_position_;
  if (media_duration_ > Clock::duration::zero() &&
      start_position_ >= media_duration_) {
    return;
  }

  playback_start_time_ = env_.now() + milliseconds(250);
  num_capturers_running_ = 2;
  audio_capturer_.emplace(env_, settings_.path_to_file.c_str(),
                          audio_encoder_.num_channels(),
                          audio_encoder_.sample_rate(), playback_start_time_,
                          start_position_, *this);
  video_capturer_.emplace(env_, settings_.path_to_file.c_str(),
                          playback_start_time_, start_position_, *this);
  is_playing_ = true;

  next_task_.ScheduleFromNow([this] { ControlForNetworkCongestion(); },
                             kCongestionCheckInterval);
  console_update_task_.ScheduleFromNow([this] { UpdateStatusOnConsole(); },
                                       kConsoleUpdateInterval);
}

void ControllableFileSender::StopCapturers() {
  next_task_.Cancel();
  console_update_task_.Cancel();
  audio_capturer_.reset();
  video_capturer_.reset();
  num_capturers_running_ = 0;
  is_playing_ = false;
}

void ControllableFileSender::UpdateStatusOnConsole() {
  const auto position = GetCurrentPosition();
  const auto seconds_part = to_seconds(position);
  const auto millis_part = to_milliseconds(position - seconds_part);
  fprintf(stdout,
          "\r\x1b[2K\rControllableFileSender: %01" PRId64 ".%03ds (%s, est. "
          "bandwidth: %d kbps)\n",
          static_cast<int64_t>(seconds_part.count()),
          static_cast<int>(millis_part.count()),
          is_playing_ ? "playing" : "paused", bandwidth_estimate_ / 1024);
  fflush(stdout);

  if (is_playing_) {
    console_update_task_.ScheduleFromNow([this] { UpdateStatusOnConsole(); },
                                         kConsoleUpdateInterval);
  }
}

Clock::duration ControllableFileSender::ClampPosition(
    Clock::duration position) const {
  if (position < Clock::duration::zero()) {
    return Clock::duration::zero();
  }
  if (media_duration_ > Clock::duration::zero() && position > media_duration_) {
    return media_duration_;
  }
  return position;
}

VideoViewport ControllableFileSender::ClampViewport(
    const VideoViewport& viewport) const {
  VideoViewport result = viewport;
  result.zoom = std::clamp(result.zoom, 1.0, 8.0);
  result.center_x = std::clamp(result.center_x, 0.0, 1.0);
  result.center_y = std::clamp(result.center_y, 0.0, 1.0);
  return result;
}

void ControllableFileSender::OnAudioData(const float* interleaved_samples,
                                         int num_samples,
                                         Clock::time_point capture_begin_time,
                                         Clock::time_point capture_end_time,
                                         Clock::time_point reference_time) {
  last_known_position_ = ClampPosition(
      start_position_ +
      std::max(reference_time - playback_start_time_, Clock::duration::zero()));
  // Empirically measured A/V sync correction: audio arrives ~30ms late
  // relative to video at the Cast receiver, verified with a sync test
  // video (simultaneous beep + flash).  The Opus codec_delay_ and
  // resampler swr_get_delay are already compensated internally, so this
  // residual offset likely comes from audio frame buffering in the
  // encode/send pipeline.
  constexpr auto kAudioSyncOffset = std::chrono::milliseconds(30);
  audio_encoder_.EncodeAndSend(interleaved_samples, num_samples,
                               capture_begin_time - kAudioSyncOffset,
                               capture_end_time - kAudioSyncOffset,
                               reference_time - kAudioSyncOffset);
}

void ControllableFileSender::OnVideoFrame(const AVFrame& av_frame,
                                          Clock::time_point capture_begin_time,
                                          Clock::time_point capture_end_time,
                                          Clock::time_point reference_time) {
  last_known_position_ = ClampPosition(
      start_position_ +
      std::max(reference_time - playback_start_time_, Clock::duration::zero()));

#if defined(__ANDROID__) && !defined(CAST_STANDALONE_SENDER_HAVE_MEDIACODEC)
  // Drop frames on Android with software encoding to reduce CPU load.
  ++video_frame_count_;
  if (video_frame_count_ % 3 != 0) {
    return;
  }
#endif

  StreamingVideoEncoder::VideoFrame frame{};
  frame.capture_begin_time = capture_begin_time;
  frame.capture_end_time = capture_end_time;
  PrepareBaseVideoFrame(av_frame, &frame);
  ApplyViewportTransform(&frame);
  video_encoder_->EncodeAndSend(frame, reference_time, {});
}

void ControllableFileSender::OnEndOfFile(SimulatedCapturer* capturer) {
  --num_capturers_running_;
  if (num_capturers_running_ == 0) {
    StopCapturers();
    last_known_position_ = media_duration_;
  }
}

void ControllableFileSender::OnError(SimulatedCapturer* capturer,
                                     const std::string& message) {
  OSP_LOG_ERROR << "Controllable sender failed: " << message;
  StopCapturers();
  if (shutdown_callback_) {
    shutdown_callback_();
  }
}

std::unique_ptr<StreamingVideoEncoder> ControllableFileSender::CreateVideoEncoder(
    const StreamingVideoEncoder::Parameters& params,
    TaskRunner& task_runner,
    std::unique_ptr<Sender> sender) {
#if defined(__ANDROID__) && defined(CAST_STANDALONE_SENDER_HAVE_MEDIACODEC)
  if (params.codec == VideoCodec::kH264) {
    return std::make_unique<StreamingMediaCodecEncoder>(params, task_runner,
                                                       std::move(sender));
  }
#endif
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
      OSP_LOG_FATAL << "AV1 codec selected, but LibAOM is unavailable.";
      return nullptr;
#endif
    default:
      OSP_LOG_ERROR << "Unsupported codec " << CodecToString(params.codec);
      OSP_NOTREACHED();
  }
}

void ControllableFileSender::PrepareBaseVideoFrame(
    const AVFrame& av_frame,
    StreamingVideoEncoder::VideoFrame* frame) {
  const int src_w = av_frame.width - av_frame.crop_left - av_frame.crop_right;
  const int src_h = av_frame.height - av_frame.crop_top - av_frame.crop_bottom;
  const uint8_t* src_y = av_frame.data[0] + av_frame.crop_left +
                         av_frame.linesize[0] * av_frame.crop_top;
  const uint8_t* src_u = av_frame.data[1] + av_frame.crop_left / 2 +
                         av_frame.linesize[1] * av_frame.crop_top / 2;
  const uint8_t* src_v = av_frame.data[2] + av_frame.crop_left / 2 +
                         av_frame.linesize[2] * av_frame.crop_top / 2;

  std::memset(padded_y_.data(), 16, padded_y_.size());
  std::memset(padded_u_.data(), 128, padded_u_.size());
  std::memset(padded_v_.data(), 128, padded_v_.size());

  int dst_w = src_w;
  int dst_h = src_h;
  if (src_w * kDisplayHeight > src_h * kDisplayWidth) {
    dst_w = kDisplayWidth;
    dst_h = RoundToEven(src_h * kDisplayWidth / src_w);
  } else {
    dst_h = kDisplayHeight;
    dst_w = RoundToEven(src_w * kDisplayHeight / src_h);
  }

  const int x_off = (kDisplayWidth - dst_w) / 2;
  const int y_off = (kDisplayHeight - dst_h) / 2;

  viewport_scaler_ = sws_getCachedContext(
      viewport_scaler_, src_w, src_h, AV_PIX_FMT_YUV420P, dst_w, dst_h,
      AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
  OSP_CHECK(viewport_scaler_);

  const uint8_t* src_planes[] = {src_y, src_u, src_v};
  const int src_strides[] = {
      av_frame.linesize[0], av_frame.linesize[1], av_frame.linesize[2]};
  uint8_t* dst_planes[] = {
      padded_y_.data() + y_off * kDisplayWidth + x_off,
      padded_u_.data() + (y_off / 2) * (kDisplayWidth / 2) + x_off / 2,
      padded_v_.data() + (y_off / 2) * (kDisplayWidth / 2) + x_off / 2,
  };
  const int dst_strides[] = {kDisplayWidth, kDisplayWidth / 2,
                             kDisplayWidth / 2};

  sws_scale(viewport_scaler_, src_planes, src_strides, 0, src_h, dst_planes,
            dst_strides);

  frame->width = kDisplayWidth;
  frame->height = kDisplayHeight;
  frame->duration = milliseconds(33);
  frame->yuv_planes[0] = padded_y_.data();
  frame->yuv_planes[1] = padded_u_.data();
  frame->yuv_planes[2] = padded_v_.data();
  frame->yuv_strides[0] = kDisplayWidth;
  frame->yuv_strides[1] = kDisplayWidth / 2;
  frame->yuv_strides[2] = kDisplayWidth / 2;
}

void ControllableFileSender::ApplyViewportTransform(
    StreamingVideoEncoder::VideoFrame* frame) {
  if (viewport_.zoom <= 1.001) {
    return;
  }

  const int crop_w =
      std::max(RoundToEven(static_cast<int>(kDisplayWidth / viewport_.zoom)), 2);
  const int crop_h =
      std::max(RoundToEven(static_cast<int>(kDisplayHeight / viewport_.zoom)), 2);
  const int max_x = kDisplayWidth - crop_w;
  const int max_y = kDisplayHeight - crop_h;
  const int crop_x = std::clamp(
      RoundToEven(static_cast<int>(viewport_.center_x * kDisplayWidth) -
                  crop_w / 2),
      0, max_x);
  const int crop_y = std::clamp(
      RoundToEven(static_cast<int>(viewport_.center_y * kDisplayHeight) -
                  crop_h / 2),
      0, max_y);

  viewport_scaler_ = sws_getCachedContext(
      viewport_scaler_, crop_w, crop_h, AV_PIX_FMT_YUV420P, kDisplayWidth,
      kDisplayHeight, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr,
      nullptr);
  OSP_CHECK(viewport_scaler_);

  const uint8_t* src_planes[] = {
      padded_y_.data() + crop_y * kDisplayWidth + crop_x,
      padded_u_.data() + (crop_y / 2) * (kDisplayWidth / 2) + crop_x / 2,
      padded_v_.data() + (crop_y / 2) * (kDisplayWidth / 2) + crop_x / 2,
  };
  const int src_strides[] = {kDisplayWidth, kDisplayWidth / 2,
                             kDisplayWidth / 2};
  uint8_t* dst_planes[] = {transformed_y_.data(), transformed_u_.data(),
                           transformed_v_.data()};
  const int dst_strides[] = {kDisplayWidth, kDisplayWidth / 2,
                             kDisplayWidth / 2};

  sws_scale(viewport_scaler_, src_planes, src_strides, 0, crop_h, dst_planes,
            dst_strides);
  frame->yuv_planes[0] = transformed_y_.data();
  frame->yuv_planes[1] = transformed_u_.data();
  frame->yuv_planes[2] = transformed_v_.data();
}

}  // namespace openscreen::cast
