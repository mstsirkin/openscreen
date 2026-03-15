// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/ffmpeg_glue.h"

#include <fcntl.h>
#include <libavcodec/version.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "util/osp_logging.h"
#include "util/std_util.h"

namespace openscreen::cast {
namespace internal {

namespace {

// Custom AVIO callbacks for reading from a file descriptor.  Used on
// Android where /proc/self/fd/ symlinks are blocked by SELinux for
// content-provider file descriptors.
int FdRead(void* opaque, uint8_t* buf, int buf_size) {
  int fd = static_cast<int>(reinterpret_cast<intptr_t>(opaque));
  int n = read(fd, buf, buf_size);
  return n == 0 ? AVERROR_EOF : (n < 0 ? AVERROR(errno) : n);
}

int64_t FdSeek(void* opaque, int64_t offset, int whence) {
  int fd = static_cast<int>(reinterpret_cast<intptr_t>(opaque));
  if (whence == AVSEEK_SIZE) {
    struct stat st;
    if (fstat(fd, &st) < 0) return AVERROR(errno);
    return st.st_size;
  }
  int64_t pos = lseek(fd, offset, whence);
  return pos < 0 ? AVERROR(errno) : pos;
}

}  // namespace

AVFormatContext* CreateAVFormatContextForFile(const char* path) {
  AVFormatContext* format_context = nullptr;
#if LIBAVCODEC_VERSION_MAJOR < 59
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  av_register_all();
#pragma GCC diagnostic pop
#endif  // LIBAVCODEC_VERSION_MAJOR < 59

  // Check for fd:<number> path — open the fd directly via custom AVIO
  // to bypass /proc/self/fd/ SELinux restrictions on Android.
  if (strncmp(path, "fd:", 3) == 0) {
    // Format: "fd:N,M" — two independent fds for the same file.
    // Each call to this function consumes the next fd so that
    // audio and video capturers get independent file offsets.
    // The counter resets when the fd values change (new video).
    const char* p = path + 3;
    int fd1 = atoi(p);
    int fd2 = fd1;
    const char* comma = strchr(p, ',');
    if (comma) {
      fd2 = atoi(comma + 1);
    }
    static int prev_fd1 = -1;
    static int call_index = 0;
    if (fd1 != prev_fd1) {
      call_index = 0;
      prev_fd1 = fd1;
    }
    int fd = (call_index++ % 2 == 0) ? fd1 : fd2;
    if (fd < 0) {
      OSP_LOG_ERROR << "Invalid fd path: " << path;
      return nullptr;
    }
    lseek(fd, 0, SEEK_SET);

    constexpr int kBufSize = 32768;
    auto* buf = static_cast<uint8_t*>(av_malloc(kBufSize));
    auto* opaque = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
    AVIOContext* avio = avio_alloc_context(
        buf, kBufSize, 0, opaque, FdRead, nullptr, FdSeek);
    if (!avio) {
      av_free(buf);
      OSP_LOG_ERROR << "Failed to create AVIO context for fd " << fd;
      return nullptr;
    }

    format_context = avformat_alloc_context();
    format_context->pb = avio;
    format_context->flags |= AVFMT_FLAG_CUSTOM_IO;

    int result = avformat_open_input(&format_context, nullptr, nullptr, nullptr);
    if (result < 0) {
      OSP_LOG_ERROR << "Cannot open fd " << fd << ": "
                    << AvErrorToString(result);
      return nullptr;
    }
  } else {
    int result = avformat_open_input(&format_context, path, nullptr, nullptr);
    if (result < 0) {
      OSP_LOG_ERROR << "Cannot open " << path << ": "
                    << AvErrorToString(result);
      return nullptr;
    }
  }

  int result = avformat_find_stream_info(format_context, nullptr);
  if (result < 0) {
    avformat_close_input(&format_context);
    OSP_LOG_ERROR << "Cannot find stream info in " << path << ": "
                  << AvErrorToString(result);
    return nullptr;
  }
  return format_context;
}

}  // namespace internal

std::string AvErrorToString(int error_num) {
  std::string out(AV_ERROR_MAX_STRING_SIZE, '\0');
  av_make_error_string(data(out), out.length(), error_num);
  return out;
}

Clock::duration GetMediaDuration(const char* path) {
  const AVFormatContextUniquePtr format_context = MakeUniqueAVFormatContext(path);
  if (!format_context || format_context->duration <= 0) {
    return Clock::duration::zero();
  }

  return Clock::duration(av_rescale_q(
      format_context->duration, AVRational{1, AV_TIME_BASE},
      AVRational{Clock::duration::period::num, Clock::duration::period::den}));
}

}  // namespace openscreen::cast
