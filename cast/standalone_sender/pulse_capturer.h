// PulseAudio monitor capturer for x11cast.
// Captures system audio output as float samples.

#ifndef CAST_STANDALONE_SENDER_PULSE_CAPTURER_H_
#define CAST_STANDALONE_SENDER_PULSE_CAPTURER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include "platform/api/time.h"
#include "util/alarm.h"

namespace openscreen::cast {

class Environment;

// Captures audio from PulseAudio monitor (system output) and delivers
// interleaved float samples to a callback.
class PulseCapturer {
 public:
  using AudioCallback = std::function<void(const float* interleaved_samples,
                                           int num_samples,
                                           Clock::time_point capture_begin,
                                           Clock::time_point capture_end,
                                           Clock::time_point reference_time)>;

  PulseCapturer(Environment& env, int num_channels, int sample_rate,
                AudioCallback callback);
  ~PulseCapturer();

 private:
  void CaptureThread();

  Environment& env_;
  int num_channels_;
  int sample_rate_;
  AudioCallback callback_;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_PULSE_CAPTURER_H_
