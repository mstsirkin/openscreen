// x11cast - Cast X11 desktop to Chromecast/Google TV using the native
// Cast protocol (mDNS discovery, TLS control, RTP/UDP streaming).
//
// Usage:
//   x11cast <network_interface_or_ip[:port]>
//
// Examples:
//   x11cast eth0                    # discover receivers on eth0
//   x11cast 192.168.1.189:8009     # connect directly to a receiver

#include <csignal>

#include "platform/impl/logging.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "cast/common/public/trust_store.h"
#include "cast/standalone_sender/constants.h"
#include "cast/standalone_sender/receiver_chooser.h"
#include "cast/standalone_sender/x11_cast_agent.h"
#include "cast/streaming/public/constants.h"
#include "platform/api/network_interface.h"
#include "platform/base/error.h"
#include "platform/base/ip_address.h"
#include "platform/impl/network_interface.h"
#include "platform/impl/platform_client_posix.h"
#include "platform/impl/task_runner.h"
#include "platform/impl/text_trace_logging_platform.h"
#include "util/string_parse.h"
#include "util/stringprintf.h"

namespace openscreen::cast {
namespace {

void LogUsage(const char* argv0) {
  std::cerr << "Usage: " << argv0 << " [options] <interface_or_ip[:port]>\n"
            << "\n"
            << "Options:\n"
            << "  -a    Use android RTP hack for older receivers\n"
            << "  -c <codec>  Video codec: vp8 (default), vp9\n"
            << "  -m <N>      Max bitrate (default: " << kDefaultMaxBitrate << ")\n"
            << "  -w <id>     Capture window by X11 window ID (hex or decimal)\n"
            << "  -v    Verbose logging\n"
            << "  -h    Show this help\n";
}

IPEndpoint ParseAsEndpoint(const char* s) {
  IPEndpoint result{};
  auto parsed = IPEndpoint::Parse(s);
  if (parsed.is_value()) {
    result = parsed.value();
  } else {
    auto addr = IPAddress::Parse(s);
    if (addr.is_value()) {
      result = {addr.value(), kDefaultCastPort};
    }
  }
  return result;
}

int X11CastMain(int argc, char* argv[]) {
  bool verbose = false;
  bool android_hack = false;
  int max_bitrate = kDefaultMaxBitrate;
  VideoCodec codec = VideoCodec::kVp8;
  unsigned long window_id = 0;

  int opt;
  while ((opt = getopt(argc, argv, "ac:m:w:vh")) != -1) {
    switch (opt) {
      case 'a': android_hack = true; break;
      case 'c':
        if (strcmp(optarg, "vp9") == 0) codec = VideoCodec::kVp9;
        else if (strcmp(optarg, "vp8") == 0) codec = VideoCodec::kVp8;
        else {
          std::cerr << "Unsupported codec: " << optarg << "\n";
          return 1;
        }
        break;
      case 'm':
        max_bitrate = atoi(optarg);
        break;
      case 'w':
        window_id = strtoul(optarg, nullptr, 0);
        break;
      case 'v': verbose = true; break;
      case 'h': LogUsage(argv[0]); return 0;
      default: LogUsage(argv[0]); return 1;
    }
  }

  if (optind >= argc) {
    LogUsage(argv[0]);
    return 1;
  }
  const char* target = argv[optind];

  SetLogLevel(verbose ? LogLevel::kVerbose : LogLevel::kInfo);

  auto cast_trust_store = CastTrustStore::Create();
  auto* task_runner = new TaskRunnerImpl(&Clock::now);
  PlatformClientPosix::Create(milliseconds(50),
                              std::unique_ptr<TaskRunnerImpl>(task_runner));

  IPEndpoint remote_endpoint = ParseAsEndpoint(target);
  if (!remote_endpoint.port) {
    // Discovery mode
    for (const InterfaceInfo& iface : GetNetworkInterfaces()) {
      if (iface.name == target) {
        ReceiverChooser chooser(iface, *task_runner,
                                [&](IPEndpoint ep) {
                                  remote_endpoint = ep;
                                  task_runner->RequestStopSoon();
                                });
        task_runner->RunUntilSignaled();
        break;
      }
    }

    if (!remote_endpoint.port) {
      std::cerr << "No Cast Receiver chosen. Cannot continue.\n";
      LogUsage(argv[0]);
      return 2;
    }
  }

  std::unique_ptr<X11CastAgent> agent;
  task_runner->PostTask([&] {
    agent = std::make_unique<X11CastAgent>(
        *task_runner, std::move(cast_trust_store),
        [&] { task_runner->RequestStopSoon(); });

    ConnectionSettings settings;
    settings.receiver_endpoint = remote_endpoint;
    settings.max_bitrate = max_bitrate;
    settings.should_include_video = true;
    settings.use_android_rtp_hack = android_hack;
    settings.use_remoting = false;
    settings.should_loop_video = false;
    settings.codec = codec;
    settings.enable_dscp = true;

    agent->Connect(std::move(settings), window_id);
  });

  task_runner->RunUntilSignaled();

  // The capture threads are still posting tasks. Trying to cleanly
  // destroy everything races with those posts. Just exit.
  OSP_LOG_INFO << "Bye!";
  exit(0);
}

}  // namespace
}  // namespace openscreen::cast

int main(int argc, char* argv[]) {
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, [](int) { _exit(0); });
  std::signal(SIGTERM, [](int) { _exit(0); });
  return openscreen::cast::X11CastMain(argc, argv);
}
