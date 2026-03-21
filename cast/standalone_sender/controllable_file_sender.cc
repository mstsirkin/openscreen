// Copyright 2026

#include "cast/standalone_sender/controllable_file_sender.h"

#include <cinttypes>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
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

constexpr auto kPausedKeepaliveInterval = std::chrono::seconds(4);

constexpr char kDirectVideoMode[] = "direct-h264-video passthrough";
constexpr char kDirectVideoArmedMode[] = "direct-h264-video passthrough (armed)";

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
      video_sender_(std::move(senders.video_sender)),
      next_task_(env_.now_function(), env_.task_runner()),
      console_update_task_(env_.now_function(), env_.task_runner()),
      paused_keepalive_task_(env_.now_function(), env_.task_runner()),
      passthrough_retry_task_(env_.now_function(), env_.task_runner()),
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

  can_passthrough_video_ = CanUseVideoPassthrough();
  if (video_sender_) {
    video_sender_->SetObserver(this);
  }
  if (!can_passthrough_video_) {
    EnsureVideoEncoderCreated();
  }

  UpdateEncoderBitrates();
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
  if (passthrough_active_) {
    last_known_position_ = GetCurrentPosition();
    FallbackToTranscode("pause", last_known_position_, false);
    return;
  }
  if (!is_playing_) {
    SchedulePausedKeepalive();
    return;
  }
  last_known_position_ = GetCurrentPosition();
  // Stop alarms and mark as paused, but keep capturers alive
  // so SeekTo can reuse them for preview frames.
  next_task_.Cancel();
  console_update_task_.Cancel();
  if (video_capturer_.has_value()) video_capturer_->SetPlaybackRate(0);
  if (audio_capturer_.has_value()) audio_capturer_->SetPlaybackRate(0);
  is_playing_ = false;
  SchedulePausedKeepalive();
}

void ControllableFileSender::Stop() {
  Pause();
  last_known_position_ = Clock::duration::zero();
}

void ControllableFileSender::SeekTo(Clock::duration position) {
  last_known_position_ = ClampPosition(position);
  if (passthrough_active_) {
    if (CanStartVideoPassthroughAt(last_known_position_)) {
      StartPlaybackAt(last_known_position_);
    } else {
      FallbackToTranscode("seek", last_known_position_, is_playing_);
    }
    return;
  }
  if (is_playing_) {
    StartPlaybackAt(last_known_position_);
  } else if (video_capturer_.has_value()) {
    // Decode and send exactly one video frame at the new position.
    auto ref = env_.now() + settings_.playout_delay;
    video_capturer_->SeekAndDeliverOneFrame(last_known_position_, ref);
    SchedulePausedKeepalive();
  } else {
    StartPausedKeepaliveAt(last_known_position_);
  }
}


void ControllableFileSender::SeekBy(Clock::duration delta) {
  SeekTo(GetCurrentPosition() + delta);
}

void ControllableFileSender::SetViewport(const VideoViewport& viewport) {
  viewport_ = ClampViewport(viewport);
  if (passthrough_active_ && !IsViewportIdentity()) {
    FallbackToTranscode("viewport", GetCurrentPosition(), is_playing_);
  }
}

void ControllableFileSender::ResetViewport() {
  viewport_ = VideoViewport{};
}

void ControllableFileSender::SetAvSyncOffset(Clock::duration offset) {
  settings_.av_sync_offset = offset;
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
  if (video_encoder_) {
    video_encoder_->SetTargetBitrate(bandwidth_being_utilized_ -
                                     audio_encoder_.GetBitrate());
  }
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

  // Wait for the playout delay duration before starting — this gives
  // time for in-flight frames from before pause to be ACK'd, and
  // matches the receiver's buffer size.
  playback_start_time_ = env_.now() + settings_.playout_delay;


  num_capturers_running_ = 1;
  audio_capturer_.emplace(env_, settings_.path_to_file.c_str(),
                          audio_encoder_.num_channels(),
                          audio_encoder_.sample_rate(), playback_start_time_,
                          start_position_, *this);
  if (CanStartVideoPassthroughAt(start_position_)) {
    video_passthrough_capturer_.emplace(env_, settings_.path_to_file.c_str(),
                                        playback_start_time_, start_position_,
                                        *this);
    passthrough_active_ = true;
    active_mode_ = kDirectVideoMode;
  } else if (settings_.should_include_video) {
    EnsureVideoEncoderCreated();
    video_capturer_.emplace(env_, settings_.path_to_file.c_str(),
                            playback_start_time_, start_position_, *this);
    passthrough_active_ = false;
    if (active_mode_.empty()) {
      active_mode_ = "fallback transcode";
    }
  } else {
    passthrough_active_ = false;
    active_mode_ = "audio only";
  }
  if (settings_.should_include_video) {
    ++num_capturers_running_;
  }
  OSP_LOG_INFO << "ControllableFileSender active mode: " << active_mode_;
  is_playing_ = true;

  next_task_.ScheduleFromNow([this] { ControlForNetworkCongestion(); },
                             kCongestionCheckInterval);
  console_update_task_.ScheduleFromNow([this] { UpdateStatusOnConsole(); },
                                       kConsoleUpdateInterval);
}

void ControllableFileSender::StartPausedKeepaliveAt(Clock::duration position) {
  StopCapturers();
  if (can_passthrough_video_ && IsViewportIdentity() && video_sender_ &&
      !video_encoder_) {
    start_position_ = ClampPosition(position);
    last_known_position_ = start_position_;
    playback_start_time_ = env_.now() + settings_.playout_delay;
    num_capturers_running_ = 0;
    is_playing_ = false;
    passthrough_active_ = false;
    active_mode_ = kDirectVideoArmedMode;
    OSP_LOG_INFO << "ControllableFileSender active mode: " << active_mode_;
    SchedulePausedKeepalive();
    return;
  }
  EnsureVideoEncoderCreated();
  start_position_ = ClampPosition(position);
  last_known_position_ = start_position_;
  playback_start_time_ = env_.now() + settings_.playout_delay;
  video_capturer_.emplace(env_, settings_.path_to_file.c_str(),
                          playback_start_time_, start_position_, *this);
  video_capturer_->SetPlaybackRate(0);
  num_capturers_running_ = 1;
  is_playing_ = false;
  passthrough_active_ = false;
  active_mode_ = "fallback transcode";
  OSP_LOG_INFO << "ControllableFileSender active mode: " << active_mode_;
  SendPausedKeepaliveFrame();
}

void ControllableFileSender::StopCapturers() {
  next_task_.Cancel();
  console_update_task_.Cancel();
  paused_keepalive_task_.Cancel();
  passthrough_retry_task_.Cancel();
  audio_capturer_.reset();
  video_capturer_.reset();
  video_passthrough_capturer_.reset();
  video_passthrough_probe_capturer_.reset();
  pending_passthrough_packet_.reset();
  num_capturers_running_ = 0;
  is_playing_ = false;
  passthrough_active_ = false;
  passthrough_backpressured_ = false;
  passthrough_reentry_pending_ = false;
}

bool ControllableFileSender::CanUseVideoPassthrough() {
  if (!settings_.should_include_video || settings_.codec != VideoCodec::kH264 ||
      !video_sender_) {
    active_mode_ = "fallback transcode: sender codec";
    return false;
  }

  const auto info =
      SimulatedVideoPassthroughCapturer::Probe(settings_.path_to_file.c_str());
  if (!info.eligible) {
    active_mode_ = "fallback transcode: " + info.reason;
    return false;
  }

  return true;
}

bool ControllableFileSender::CanStartVideoPassthroughAt(
    Clock::duration position) const {
  return settings_.should_include_video && can_passthrough_video_ &&
         IsViewportIdentity() &&
         (media_duration_ <= Clock::duration::zero() || position < media_duration_);
}

bool ControllableFileSender::IsViewportIdentity() const {
  return viewport_.zoom <= 1.001 && std::abs(viewport_.center_x - 0.5) < 1e-6 &&
         std::abs(viewport_.center_y - 0.5) < 1e-6;
}

void ControllableFileSender::EnsureVideoEncoderCreated() {
  if (video_encoder_ || !video_sender_) {
    return;
  }
  video_encoder_ = CreateVideoEncoder(
      StreamingVideoEncoder::Parameters{.codec = settings_.codec},
      env_.task_runner(), std::move(video_sender_));
}

void ControllableFileSender::FallbackToTranscode(const char* reason,
                                                 Clock::duration position,
                                                 bool resume_playback,
                                                 bool disable_passthrough) {
  if (!can_passthrough_video_) {
    return;
  }
  OSP_LOG_INFO << "Passthrough fallback: " << reason;
  if (disable_passthrough) {
    can_passthrough_video_ = false;
  }
  passthrough_active_ = false;
  active_mode_ = std::string("fallback transcode: ") + reason;
  EnsureVideoEncoderCreated();
  if (resume_playback) {
    StartPlaybackAt(position);
    if (!disable_passthrough && can_passthrough_video_ && IsViewportIdentity()) {
      StartPassthroughReentryProbe(position);
    }
  } else {
    StartPausedKeepaliveAt(position);
  }
}

void ControllableFileSender::StartPassthroughReentryProbe(
    Clock::duration position) {
  if (!can_passthrough_video_ || !IsViewportIdentity() || !is_playing_) {
    video_passthrough_probe_capturer_.reset();
    passthrough_reentry_pending_ = false;
    return;
  }
  video_passthrough_probe_capturer_.emplace(
      env_, settings_.path_to_file.c_str(), playback_start_time_,
      ClampPosition(position), *this);
  passthrough_reentry_pending_ = true;
  OSP_LOG_INFO << "Passthrough re-entry probe armed at "
               << to_milliseconds(ClampPosition(position)).count() << "ms";
}

void ControllableFileSender::SchedulePausedKeepalive() {
  paused_keepalive_task_.Cancel();
  if (is_playing_) {
    return;
  }
  paused_keepalive_task_.ScheduleFromNow(
      [this] { SendPausedKeepaliveFrame(); }, kPausedKeepaliveInterval);
}

void ControllableFileSender::SendPausedKeepaliveFrame() {
  if (is_playing_) {
    return;
  }
  if (passthrough_active_) {
    FallbackToTranscode("paused keepalive", last_known_position_, false);
    return;
  }
  if (!video_capturer_.has_value()) {
    if (can_passthrough_video_ && !video_encoder_) {
      FallbackToTranscode("paused keepalive", last_known_position_, false);
      return;
    }
    StartPausedKeepaliveAt(last_known_position_);
    return;
  }
  const auto ref = env_.now() + settings_.playout_delay;
  video_capturer_->SeekAndDeliverOneFrame(last_known_position_, ref);
  SchedulePausedKeepalive();
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
  const auto computed_position = ClampPosition(
      start_position_ +
      std::max(reference_time - playback_start_time_, Clock::duration::zero()));
  if (is_playing_) {
    last_known_position_ = computed_position;
  }
  // Apply A/V sync correction. Sender-side pipelines are well-synchronized
  // (< 1ms wall delay for both), but receivers have different audio vs video
  // decode/render latencies. This offset shifts audio earlier to compensate.
  const auto offset = settings_.av_sync_offset;
  audio_encoder_.EncodeAndSend(interleaved_samples, num_samples,
                               capture_begin_time - offset,
                               capture_end_time - offset,
                               reference_time - offset);
}

void ControllableFileSender::OnVideoFrame(const AVFrame& av_frame,
                                          Clock::time_point capture_begin_time,
                                          Clock::time_point capture_end_time,
                                          Clock::time_point reference_time) {
  const auto computed_position = ClampPosition(
      start_position_ +
      std::max(reference_time - playback_start_time_, Clock::duration::zero()));
  if (is_playing_) {
    last_known_position_ = computed_position;
  }

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

void ControllableFileSender::OnVideoPacket(
    std::vector<uint8_t> data,
    bool is_key_frame,
    Clock::duration media_timestamp,
    Clock::duration media_duration,
    Clock::time_point capture_begin_time,
    Clock::time_point capture_end_time,
    Clock::time_point reference_time) {
  if (passthrough_reentry_pending_ && !passthrough_active_) {
    if (is_key_frame) {
      OSP_LOG_INFO << "Passthrough re-entry at "
                   << to_milliseconds(media_timestamp).count() << "ms";
      StartPlaybackAt(media_timestamp);
    } else if (video_passthrough_probe_capturer_.has_value()) {
      video_passthrough_probe_capturer_->Continue();
    }
    return;
  }
  pending_passthrough_packet_ = PendingPassthroughPacket{
      .data = std::move(data),
      .is_key_frame = is_key_frame,
      .media_timestamp = media_timestamp,
      .media_duration = media_duration,
      .capture_begin_time = capture_begin_time,
      .capture_end_time = capture_end_time,
      .reference_time = reference_time,
  };
  RetryPendingPassthroughPacket();
}

void ControllableFileSender::RetryPendingPassthroughPacket() {
  if (!pending_passthrough_packet_) {
    return;
  }

  if (!video_sender_) {
    const auto media_timestamp = pending_passthrough_packet_->media_timestamp;
    pending_passthrough_packet_.reset();
    FallbackToTranscode("missing direct sender", media_timestamp, true, true);
    return;
  }

  passthrough_retry_task_.Cancel();

  const auto& pending = *pending_passthrough_packet_;
  const bool is_key_frame = pending.is_key_frame;
  const auto media_timestamp = pending.media_timestamp;
  const auto capture_begin_time = pending.capture_begin_time;
  const auto capture_end_time = pending.capture_end_time;
  const auto reference_time = pending.reference_time;
  ByteView data(pending.data.data(), pending.data.size());

  const auto computed_position = ClampPosition(
      start_position_ +
      std::max(reference_time - playback_start_time_, Clock::duration::zero()));
  if (is_playing_) {
    last_known_position_ = computed_position;
  }

  const FrameId frame_id = video_sender_->GetNextFrameId();
  if (frame_id == FrameId::first() && !is_key_frame) {
    FallbackToTranscode("non-keyframe start", computed_position, true, true);
    return;
  }
  EncodedFrame frame(
      is_key_frame ? EncodedFrame::Dependency::kKeyFrame
                   : EncodedFrame::Dependency::kDependent,
      frame_id, is_key_frame ? frame_id : frame_id - 1,
      RtpTimeTicks::FromTimeSinceOrigin(media_timestamp,
                                        video_sender_->rtp_timebase()),
      reference_time, std::chrono::milliseconds::zero(), capture_begin_time,
      capture_end_time, data);
  const auto result = video_sender_->EnqueueFrame(frame);
  if (result == Sender::OK) {
    if (passthrough_backpressured_) {
      passthrough_backpressured_ = false;
      if (audio_capturer_.has_value()) {
        audio_capturer_->SetPlaybackRate(1.0);
      }
    }
    pending_passthrough_packet_.reset();
    if (video_passthrough_capturer_.has_value()) {
      video_passthrough_capturer_->Continue();
    }
    return;
  }
  if (result == Sender::MAX_DURATION_IN_FLIGHT) {
    const auto in_flight =
        video_sender_->GetInFlightMediaDuration(frame.rtp_timestamp);
    const auto max_in_flight = video_sender_->GetMaxInFlightMediaDuration();
    if (!passthrough_backpressured_) {
      passthrough_backpressured_ = true;
      if (audio_capturer_.has_value()) {
        audio_capturer_->SetPlaybackRate(0.0);
      }
    }
    OSP_LOG_WARN << "Passthrough waiting on sender backlog: inflight="
                 << in_flight << " max=" << max_in_flight;
    return;
  }

  std::ostringstream reason;
  reason << "passthrough enqueue failed: " << result;
  OSP_LOG_WARN << reason.str();
  pending_passthrough_packet_.reset();
  FallbackToTranscode(reason.str().c_str(), computed_position, is_playing(),
                      true);
}

void ControllableFileSender::OnEndOfFile(SimulatedCapturer* capturer) {
  --num_capturers_running_;
  if (num_capturers_running_ == 0) {
    StopCapturers();
    last_known_position_ = media_duration_;
    StartPausedKeepaliveAt(last_known_position_);
  }
}

void ControllableFileSender::OnEndOfFile(
    SimulatedVideoPassthroughCapturer* capturer) {
  if (video_passthrough_probe_capturer_.has_value() &&
      &*video_passthrough_probe_capturer_ == capturer) {
    video_passthrough_probe_capturer_.reset();
    passthrough_reentry_pending_ = false;
    return;
  }
  --num_capturers_running_;
  if (num_capturers_running_ == 0) {
    StopCapturers();
    last_known_position_ = media_duration_;
    StartPausedKeepaliveAt(last_known_position_);
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

void ControllableFileSender::OnError(
    SimulatedVideoPassthroughCapturer* capturer,
    const std::string& message) {
  if (video_passthrough_probe_capturer_.has_value() &&
      &*video_passthrough_probe_capturer_ == capturer) {
    OSP_LOG_WARN << "Passthrough re-entry probe failed: " << message;
    video_passthrough_probe_capturer_.reset();
    passthrough_reentry_pending_ = false;
    return;
  }
  OSP_LOG_ERROR << "Passthrough sender failed: " << message;
  FallbackToTranscode("passthrough error", last_known_position_, is_playing_,
                      true);
}

void ControllableFileSender::OnFrameCanceled(FrameId frame_id) {
  if (pending_passthrough_packet_ && passthrough_active_) {
    passthrough_retry_task_.Schedule([this] { RetryPendingPassthroughPacket(); },
                                     Alarm::kImmediately);
  }
}

void ControllableFileSender::OnPictureLost() {
}

std::string ControllableFileSender::GetActiveModeString() const {
  return active_mode_;
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
  const int unrotated_w =
      av_frame.width - av_frame.crop_left - av_frame.crop_right;
  const int unrotated_h =
      av_frame.height - av_frame.crop_top - av_frame.crop_bottom;
  const uint8_t* source_y = av_frame.data[0] + av_frame.crop_left +
                            av_frame.linesize[0] * av_frame.crop_top;
  const uint8_t* source_u = av_frame.data[1] + av_frame.crop_left / 2 +
                            av_frame.linesize[1] * av_frame.crop_top / 2;
  const uint8_t* source_v = av_frame.data[2] + av_frame.crop_left / 2 +
                            av_frame.linesize[2] * av_frame.crop_top / 2;
  int src_w = unrotated_w;
  int src_h = unrotated_h;
  int src_y_stride = av_frame.linesize[0];
  int src_u_stride = av_frame.linesize[1];
  int src_v_stride = av_frame.linesize[2];
  const uint8_t* src_y = source_y;
  const uint8_t* src_u = source_u;
  const uint8_t* src_v = source_v;

  const int rotation =
      video_capturer_ ? video_capturer_->display_rotation_degrees() : 0;

  std::memset(padded_y_.data(), 16, padded_y_.size());
  std::memset(padded_u_.data(), 128, padded_u_.size());
  std::memset(padded_v_.data(), 128, padded_v_.size());

  const bool quarter_turn = rotation == 90 || rotation == 270;
  const int display_src_w = quarter_turn ? src_h : src_w;
  const int display_src_h = quarter_turn ? src_w : src_h;

  int display_dst_w = display_src_w;
  int display_dst_h = display_src_h;
  if (display_src_w * kDisplayHeight > display_src_h * kDisplayWidth) {
    display_dst_w = kDisplayWidth;
    display_dst_h = RoundToEven(display_src_h * kDisplayWidth / display_src_w);
  } else {
    display_dst_h = kDisplayHeight;
    display_dst_w = RoundToEven(display_src_w * kDisplayHeight / display_src_h);
  }

  const int x_off = (kDisplayWidth - display_dst_w) / 2;
  const int y_off = (kDisplayHeight - display_dst_h) / 2;

  const int scale_dst_w = quarter_turn ? display_dst_h : display_dst_w;
  const int scale_dst_h = quarter_turn ? display_dst_w : display_dst_h;

  viewport_scaler_ = sws_getCachedContext(
      viewport_scaler_, src_w, src_h, AV_PIX_FMT_YUV420P, scale_dst_w,
      scale_dst_h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
  OSP_CHECK(viewport_scaler_);

  const uint8_t* src_planes[] = {src_y, src_u, src_v};
  const int src_strides[] = {src_y_stride, src_u_stride, src_v_stride};
  scaled_y_.resize(scale_dst_w * scale_dst_h);
  scaled_u_.resize((scale_dst_w / 2) * (scale_dst_h / 2));
  scaled_v_.resize((scale_dst_w / 2) * (scale_dst_h / 2));
  uint8_t* scaled_planes[] = {
      scaled_y_.data(),
      scaled_u_.data(),
      scaled_v_.data(),
  };
  const int scaled_strides[] = {scale_dst_w, scale_dst_w / 2, scale_dst_w / 2};

  sws_scale(viewport_scaler_, src_planes, src_strides, 0, src_h, scaled_planes,
            scaled_strides);

  if (rotation == 0) {
    for (int row = 0; row < scale_dst_h; ++row) {
      std::memcpy(padded_y_.data() + (y_off + row) * kDisplayWidth + x_off,
                  scaled_y_.data() + row * scale_dst_w, scale_dst_w);
    }
    for (int row = 0; row < scale_dst_h / 2; ++row) {
      std::memcpy(padded_u_.data() + (y_off / 2 + row) * (kDisplayWidth / 2) +
                      x_off / 2,
                  scaled_u_.data() + row * (scale_dst_w / 2), scale_dst_w / 2);
      std::memcpy(padded_v_.data() + (y_off / 2 + row) * (kDisplayWidth / 2) +
                      x_off / 2,
                  scaled_v_.data() + row * (scale_dst_w / 2), scale_dst_w / 2);
    }
  } else {
    RotateI420IntoPadded(rotation, scale_dst_w, scale_dst_h, x_off, y_off);
  }

  frame->width = kDisplayWidth;
  frame->height = kDisplayHeight;
  frame->duration = milliseconds(33);
  frame->rotation_degrees = 0;
  frame->yuv_planes[0] = padded_y_.data();
  frame->yuv_planes[1] = padded_u_.data();
  frame->yuv_planes[2] = padded_v_.data();
  frame->yuv_strides[0] = kDisplayWidth;
  frame->yuv_strides[1] = kDisplayWidth / 2;
  frame->yuv_strides[2] = kDisplayWidth / 2;
}

void ControllableFileSender::RotateI420IntoPadded(int rotation_degrees,
                                                  int src_w,
                                                  int src_h,
                                                  int dst_x,
                                                  int dst_y_offset) {
  auto rotate_plane = [&](const uint8_t* src,
                          int src_stride,
                          int width,
                          int height,
                          uint8_t* dst,
                          int dst_stride) {
    switch (rotation_degrees) {
      case 90:
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
            dst[x * dst_stride + (height - 1 - y)] = src[y * src_stride + x];
          }
        }
        break;
      case 180:
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
            dst[(height - 1 - y) * dst_stride + (width - 1 - x)] =
                src[y * src_stride + x];
          }
        }
        break;
      case 270:
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
            dst[(width - 1 - x) * dst_stride + y] = src[y * src_stride + x];
          }
        }
        break;
      default:
        break;
    }
  };

  uint8_t* dst_y = padded_y_.data() + dst_y_offset * kDisplayWidth + dst_x;
  uint8_t* dst_u =
      padded_u_.data() + (dst_y_offset / 2) * (kDisplayWidth / 2) + dst_x / 2;
  uint8_t* dst_v =
      padded_v_.data() + (dst_y_offset / 2) * (kDisplayWidth / 2) + dst_x / 2;
  rotate_plane(scaled_y_.data(), src_w, src_w, src_h, dst_y, kDisplayWidth);
  rotate_plane(scaled_u_.data(), src_w / 2, src_w / 2, src_h / 2, dst_u,
               kDisplayWidth / 2);
  rotate_plane(scaled_v_.data(), src_w / 2, src_w / 2, src_h / 2, dst_v,
               kDisplayWidth / 2);
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
