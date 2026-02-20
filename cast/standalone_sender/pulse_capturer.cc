// PulseAudio monitor capturer implementation.
// Uses pa_simple API to capture from the default monitor source.

#include "cast/standalone_sender/pulse_capturer.h"

#include <pulse/error.h>
#include <pulse/simple.h>

#include <cstring>
#include <vector>

#include "cast/streaming/public/environment.h"
#include "util/osp_logging.h"

namespace openscreen::cast {

PulseCapturer::PulseCapturer(Environment& env, int num_channels,
                             int sample_rate, AudioCallback callback)
    : env_(env),
      num_channels_(num_channels),
      sample_rate_(sample_rate),
      callback_(std::move(callback)) {
  thread_ = std::thread(&PulseCapturer::CaptureThread, this);
}

PulseCapturer::~PulseCapturer() {
  running_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
}

void PulseCapturer::CaptureThread() {
  // PulseAudio sample spec: float32, stereo, at the requested rate
  pa_sample_spec spec{};
  spec.format = PA_SAMPLE_FLOAT32LE;
  spec.channels = static_cast<uint8_t>(num_channels_);
  spec.rate = static_cast<uint32_t>(sample_rate_);

  int error = 0;
  // Find the monitor source for the default sink.
  // The monitor source name is the default sink name + ".monitor".
  // This captures system audio output, not the microphone.
  std::string monitor_source;
  FILE* fp = popen("pactl get-default-sink 2>/dev/null", "r");
  if (fp) {
    char buf[256];
    if (fgets(buf, sizeof(buf), fp)) {
      monitor_source = buf;
      // Remove trailing newline
      while (!monitor_source.empty() && monitor_source.back() == '\n') {
        monitor_source.pop_back();
      }
      monitor_source += ".monitor";
    }
    pclose(fp);
  }

  const char* device = monitor_source.empty() ? nullptr
                                               : monitor_source.c_str();
  OSP_LOG_INFO << "PulseCapturer: using source: "
               << (device ? device : "(default)");

  // Set minimal buffer to reduce audio latency. fragsize controls how
  // much PulseAudio buffers before delivering to us.
  const int bytes_per_chunk =
      (sample_rate_ / 100) * num_channels_ * sizeof(float);
  pa_buffer_attr buf_attr{};
  buf_attr.maxlength = static_cast<uint32_t>(-1);  // default
  buf_attr.tlength = static_cast<uint32_t>(-1);     // N/A for recording
  buf_attr.prebuf = static_cast<uint32_t>(-1);      // N/A for recording
  buf_attr.minreq = static_cast<uint32_t>(-1);      // N/A for recording
  buf_attr.fragsize = bytes_per_chunk;               // deliver in 10ms chunks

  pa_simple* pa = pa_simple_new(
      nullptr,           // default server
      "x11cast",         // app name
      PA_STREAM_RECORD,  // direction
      device,            // monitor source (system audio output)
      "screen-audio",    // stream description
      &spec,             // sample format
      nullptr,           // default channel map
      &buf_attr,         // minimal buffering
      &error);

  if (!pa) {
    OSP_LOG_ERROR << "PulseCapturer: failed to open PulseAudio: "
                  << pa_strerror(error);
    return;
  }

  OSP_LOG_INFO << "PulseCapturer: capturing " << num_channels_ << "ch "
               << sample_rate_ << "Hz audio";

  // Flush any stale audio sitting in PulseAudio's buffer so our
  // timestamps match the actual capture time.
  pa_simple_flush(pa, &error);

  // Read in chunks of ~10ms worth of audio
  const int frames_per_chunk = sample_rate_ / 100;
  const int samples_per_chunk = frames_per_chunk * num_channels_;
  std::vector<float> buffer(samples_per_chunk);

  while (running_) {
    int ret = pa_simple_read(pa, buffer.data(),
                              buffer.size() * sizeof(float), &error);
    if (ret < 0) {
      OSP_LOG_ERROR << "PulseCapturer: read error: " << pa_strerror(error);
      break;
    }

    auto now = env_.now();
    callback_(buffer.data(), frames_per_chunk, now, now, now);
  }

  pa_simple_free(pa);
  OSP_LOG_INFO << "PulseCapturer: stopped";
}

}  // namespace openscreen::cast
