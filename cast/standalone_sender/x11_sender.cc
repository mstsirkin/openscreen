// X11 screen sender implementation.

#include "cast/standalone_sender/x11_sender.h"

#include <cinttypes>
#include <utility>

#if defined(CAST_STANDALONE_SENDER_HAVE_LIBAOM)
#include "cast/standalone_sender/streaming_av1_encoder.h"
#endif
#include "cast/standalone_sender/streaming_vpx_encoder.h"
#include "platform/base/trivial_clock_traits.h"
#include "util/chrono_helpers.h"
#include "util/osp_logging.h"
#include "util/trace_logging.h"

namespace openscreen::cast {

namespace {
constexpr auto kCongestionCheckInterval = milliseconds(500);
constexpr auto kConsoleUpdateInterval = milliseconds(5000);
constexpr int kHighBandwidthThreshold = 5000000;
constexpr int kMinRequiredBitrate = 300000;
}  // namespace

X11Sender::X11Sender(Environment& environment,
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
          StreamingVideoEncoder::Parameters{.codec = settings_.codec},
          env_.task_runner(),
          std::move(senders.video_sender))),
      congestion_alarm_(env_.now_function(), env_.task_runner()),
      console_alarm_(env_.now_function(), env_.task_runner()) {
  OSP_LOG_INFO << "X11Sender: starting screen capture";

  bandwidth_being_utilized_ = settings_.max_bitrate / 2;
  UpdateEncoderBitrates();

  start_time_ = env_.now();

  // Start video capturer (30 fps, desktop)
  video_capturer_ = std::make_unique<X11Capturer>(
      env_, 30,
      [this](const X11Capturer::Frame& frame, Clock::time_point ref) {
        OnVideoFrame(frame, ref);
      });

  // Start audio capturer (48kHz stereo)
  audio_capturer_ = std::make_unique<PulseCapturer>(
      env_, audio_encoder_.num_channels(), audio_encoder_.sample_rate(),
      [this](const float* samples, int num_samples,
             Clock::time_point begin, Clock::time_point end,
             Clock::time_point ref) {
        OnAudioData(samples, num_samples, begin, end, ref);
      });

  congestion_alarm_.ScheduleFromNow(
      [this] { ControlForNetworkCongestion(); }, kCongestionCheckInterval);
  console_alarm_.ScheduleFromNow(
      [this] { UpdateStatusOnConsole(); }, kConsoleUpdateInterval);
}

X11Sender::~X11Sender() {
  congestion_alarm_.Cancel();
  console_alarm_.Cancel();
  audio_capturer_.reset();
  video_capturer_.reset();
}

void X11Sender::OnVideoFrame(const X11Capturer::Frame& frame,
                              Clock::time_point reference_time) {
  StreamingVideoEncoder::VideoFrame vf{};
  vf.width = frame.width;
  vf.height = frame.height;
  vf.yuv_planes[0] = frame.y;
  vf.yuv_planes[1] = frame.u;
  vf.yuv_planes[2] = frame.v;
  vf.yuv_strides[0] = frame.y_stride;
  vf.yuv_strides[1] = frame.u_stride;
  vf.yuv_strides[2] = frame.v_stride;
  vf.duration = milliseconds(1000 / 30);
  vf.capture_begin_time = frame.capture_begin;
  vf.capture_end_time = frame.capture_end;

  video_encoder_->EncodeAndSend(vf, reference_time, {});
}

void X11Sender::OnAudioData(const float* interleaved_samples,
                             int num_samples,
                             Clock::time_point capture_begin,
                             Clock::time_point capture_end,
                             Clock::time_point reference_time) {
  // Audio data arrives from PulseAudio's capture thread.
  // Copy the data and marshal to the task runner thread, because the
  // encoder and Sender are not thread-safe.
  auto samples = std::make_shared<std::vector<float>>(
      interleaved_samples, interleaved_samples + num_samples * audio_encoder_.num_channels());
  env_.task_runner().PostTask(
      [this, samples, num_samples, capture_begin, capture_end, reference_time] {
        audio_encoder_.EncodeAndSend(samples->data(), num_samples,
                                     capture_begin, capture_end, reference_time);
      });
}

void X11Sender::ControlForNetworkCongestion() {
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

  congestion_alarm_.ScheduleFromNow(
      [this] { ControlForNetworkCongestion(); }, kCongestionCheckInterval);
}

void X11Sender::UpdateEncoderBitrates() {
  if (bandwidth_being_utilized_ >= kHighBandwidthThreshold) {
    audio_encoder_.UseHighQuality();
  } else {
    audio_encoder_.UseStandardQuality();
  }
  video_encoder_->SetTargetBitrate(bandwidth_being_utilized_ -
                                   audio_encoder_.GetBitrate());
}

void X11Sender::UpdateStatusOnConsole() {
  auto elapsed = env_.now() - start_time_;
  auto secs = to_seconds(elapsed);
  fprintf(stdout,
          "\r\x1b[2K\rX11Sender: streaming for %" PRId64
          "s (bandwidth: %d kbps)\n",
          static_cast<int64_t>(secs.count()),
          bandwidth_estimate_ / 1024);
  fflush(stdout);

  console_alarm_.ScheduleFromNow(
      [this] { UpdateStatusOnConsole(); }, kConsoleUpdateInterval);
}

std::unique_ptr<StreamingVideoEncoder> X11Sender::CreateVideoEncoder(
    const StreamingVideoEncoder::Parameters& params,
    TaskRunner& task_runner,
    std::unique_ptr<Sender> sender) {
  switch (params.codec) {
    case VideoCodec::kVp8:
    case VideoCodec::kVp9:
      return std::make_unique<StreamingVpxEncoder>(params, task_runner,
                                                   std::move(sender));
#if defined(CAST_STANDALONE_SENDER_HAVE_LIBAOM)
    case VideoCodec::kAv1:
      return std::make_unique<StreamingAv1Encoder>(params, task_runner,
                                                   std::move(sender));
#endif
    default:
      OSP_LOG_FATAL << "Unsupported codec: " << CodecToString(params.codec);
      return nullptr;
  }
}

}  // namespace openscreen::cast
