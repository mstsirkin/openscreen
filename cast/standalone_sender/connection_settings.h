// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_CONNECTION_SETTINGS_H_
#define CAST_STANDALONE_SENDER_CONNECTION_SETTINGS_H_

#include <chrono>
#include <string>

#include "cast/streaming/public/constants.h"
#include "platform/api/time.h"
#include "platform/base/interface_info.h"

namespace openscreen::cast {

// The connection settings for a given standalone sender instance. These fields
// are used throughout the standalone sender component to initialize state from
// the command line parameters.
struct ConnectionSettings {
  // The endpoint of the receiver we wish to connect to.
  IPEndpoint receiver_endpoint;

  // The path to the file that we want to play.
  std::string path_to_file;

  // The maximum bitrate. Default value means a reasonable default will be
  // selected.
  int max_bitrate = 0;

  // Whether the stream should include video, or just be audio only.
  bool should_include_video = true;

  // Whether we should use the hacky RTP stream IDs for legacy android
  // receivers, or if we should use the proper values. For more information,
  // see issuetracker.google.com/184438154.
  bool use_android_rtp_hack = true;

  // Whether we should use remoting for the video, instead of the default of
  // mirroring.
  bool use_remoting = false;

  // Whether we should loop the video when it is completed.
  bool should_loop_video = true;

  // The codec to use for encoding negotiated video streams.
  VideoCodec codec;

  // Whether DSCP support should be enabled for Quality of Service.
  bool enable_dscp = true;

  // PulseAudio source to capture audio from. Empty means default sink monitor.
  std::string pulse_source;

  // Playout delay: how long the receiver buffers before rendering.
  // Lower = more responsive, higher = more resilient to network jitter.
  Clock::duration playout_delay = std::chrono::milliseconds(400);

  // Audio-video sync offset: shifts audio reference_time earlier by this
  // amount to compensate for the receiver's audio/video render latency
  // difference. Positive = audio plays earlier. Measured empirically
  // using a sync test (simultaneous beep + flash). Default 30ms works
  // for most Cast receivers (Google TV).
  Clock::duration av_sync_offset = std::chrono::milliseconds(0);

  // Optional brightness adjustment applied on the CPU transform path.
  // Range is [-200, 200], with 0 meaning no adjustment. Non-zero brightness
  // requires transcode rather than passthrough.
  int brightness = 0;

  // Maximum time to allow the initial Cast connection setup to complete
  // before treating it as stalled and shutting the session down.
  Clock::duration connect_timeout = std::chrono::seconds(8);

  // Whether direct video passthrough is allowed at all. Disabled by default
  // so the app stays on the fallback transcode path unless explicitly enabled.
  bool enable_video_passthrough = false;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_CONNECTION_SETTINGS_H_
