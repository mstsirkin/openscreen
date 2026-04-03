// JNI bridge between the Android ControlCast app and the Open Screen
// ControllableFileCastAgent.  When built with the full Open Screen library
// (HAVE_OPENSCREEN defined), native connect/play/pause/seek/viewport calls
// are forwarded to the real Cast sender.  Otherwise a status-only stub is
// compiled so the APK can still be built and run for UI development.

#include <jni.h>
#include <unistd.h>

#include <android/log.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ControlCast", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ControlCast", __VA_ARGS__)

#ifdef HAVE_OPENSCREEN
#include "platform/impl/logging.h"
#include "cast/common/public/trust_store.h"
#include "cast/standalone_sender/connection_settings.h"
#include "cast/standalone_sender/controllable_file_cast_agent.h"
#include "cast/streaming/public/constants.h"
#include "platform/base/ip_address.h"
#include "platform/impl/platform_client_posix.h"
#include "platform/impl/task_runner.h"
#include "util/alarm.h"
#include "util/chrono_helpers.h"
#endif  // HAVE_OPENSCREEN

namespace {
constexpr auto kInitialReconnectDelay = std::chrono::milliseconds(200);
constexpr auto kMaxReconnectDelay = std::chrono::seconds(5);
constexpr size_t kPausedSeekQueueCapacity = 1;
constexpr auto kRapidSeekGap = std::chrono::milliseconds(350);
constexpr auto kSeekStormRestartCooldown = std::chrono::milliseconds(1200);
constexpr int kSeekStormThreshold = 5;

struct ConnectionState {
  std::string target;
  bool connected = false;

#ifdef HAVE_OPENSCREEN
  std::unique_ptr<openscreen::cast::ControllableFileCastAgent> cast;
  std::unique_ptr<openscreen::Alarm> reconnect_alarm;
  bool session_restart_posted = false;
  uint64_t desired_session_generation = 0;
  uint64_t cast_generation = 0;
  bool reconnect_enabled = false;
  std::chrono::milliseconds reconnect_delay = kInitialReconnectDelay;
  int rapid_seek_count = 0;
  std::chrono::steady_clock::time_point last_seek_at{};
  std::chrono::steady_clock::time_point last_seek_restart_at{};
  std::deque<long long> paused_seek_queue;
  bool paused_seek_drain_posted = false;
#endif
};

struct ControllerState {
  std::mutex mutex;
  ConnectionState connection;
  std::string video_uri;
  bool mirror_locally = true;
  bool playing = false;
  bool desired_playing = false;
  long long position_ms = 0;
  long long pending_open_position_ms = 0;
  bool has_pending_open_position = false;
  float zoom = 1.0f;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  int video_fd = -1;
  int video_fd2 = -1;
  std::string video_path;
  bool use_hw_encode = true;
  bool enable_video_passthrough = false;
  int brightness = 0;
  long long av_sync_offset_ms = 0;
  int playout_delay_ms = 400;
  int connect_timeout_ms = 8000;
  std::string active_mode = "idle";
  std::string debug_state;
  std::string status = "Native backend ready.";
  std::string last_session_end_reason = "none";

#ifdef HAVE_OPENSCREEN
  openscreen::TaskRunnerImpl* task_runner = nullptr;
  std::thread runner_thread;
#endif
};

ControllerState& State() {
  static ControllerState state;
  return state;
}

std::string JStringToStdString(JNIEnv* env, jstring value) {
  if (!value) {
    return {};
  }
  const char* chars = env->GetStringUTFChars(value, nullptr);
  std::string out = chars ? chars : "";
  if (chars) {
    env->ReleaseStringUTFChars(value, chars);
  }
  return out;
}

jstring StdStringToJString(JNIEnv* env, const std::string& value) {
  return env->NewStringUTF(value.c_str());
}

// Caller must hold state.mutex.
void UpdateStatusLocked(ControllerState& state) {
  std::ostringstream stream;
  if (!state.connection.connected) {
    stream << "Not connected.";
    if (!state.connection.target.empty()) {
      stream << " Target: " << state.connection.target;
    }
    stream << " | video pt " << (state.enable_video_passthrough ? "on" : "off");
    if (!state.active_mode.empty()) {
      stream << " | mode " << state.active_mode;
    }
    if (!state.debug_state.empty()) {
      stream << " | dbg " << state.debug_state;
    }
    if (!state.last_session_end_reason.empty()) {
      stream << " | last end " << state.last_session_end_reason;
    }
    state.status = stream.str();
    return;
  }

  stream << "Connected to " << state.connection.target;
  if (!state.video_uri.empty()) {
    stream << " | " << (state.playing ? "Playing" : "Paused");
    stream << " @ " << state.position_ms / 1000.0 << "s";
    stream << " | zoom " << state.zoom;
    stream << " | local mirror " << (state.mirror_locally ? "on" : "off");
    stream << " | video pt " << (state.enable_video_passthrough ? "on" : "off");
    if (!state.active_mode.empty()) {
      stream << " | mode " << state.active_mode;
    }
    if (!state.debug_state.empty()) {
      stream << " | dbg " << state.debug_state;
    }
  } else {
    stream << " | no video selected";
  }
  if (!state.last_session_end_reason.empty()) {
    stream << " | last end " << state.last_session_end_reason;
  }
#ifndef HAVE_OPENSCREEN
  stream << " | native Open Screen sender hookup pending";
#endif
  state.status = stream.str();
}

#ifdef HAVE_OPENSCREEN
openscreen::IPEndpoint ParseTarget(const std::string& target) {
  auto parsed = openscreen::IPEndpoint::Parse(target);
  if (parsed.is_value()) {
    return parsed.value();
  }
  auto addr = openscreen::IPAddress::Parse(target);
  if (addr.is_value()) {
    return {addr.value(), openscreen::cast::kDefaultCastPort};
  }
  return {};
}

void RequestCastSessionRestart(ControllerState& state, const char* reason);
void DrainPausedSeekQueue(ControllerState& state);

void EnsureTaskRunner(ControllerState& state) {
  if (!state.task_runner) {
    state.task_runner = new openscreen::TaskRunnerImpl(&openscreen::Clock::now);
    openscreen::PlatformClientPosix::Create(
        openscreen::milliseconds(50),
        std::unique_ptr<openscreen::TaskRunnerImpl>(state.task_runner));
    state.runner_thread = std::thread([&state]() {
      state.task_runner->RunUntilStopped();
    });
    state.runner_thread.detach();
    state.connection.reconnect_alarm =
        std::make_unique<openscreen::Alarm>(&openscreen::Clock::now,
                                            *state.task_runner);
  }
}

void StartCastSessionOnTaskRunner(ControllerState& state,
                                  openscreen::IPEndpoint endpoint,
                                  const std::string& video_path) {
  long long position_ms = 0;
  bool playing = false;
  float zoom = 1.0f;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.has_pending_open_position) {
      position_ms = state.pending_open_position_ms;
      state.position_ms = state.pending_open_position_ms;
      state.has_pending_open_position = false;
    } else {
      position_ms = state.position_ms;
    }
    playing = state.desired_playing;
    zoom = state.zoom;
    offset_x = state.offset_x;
    offset_y = state.offset_y;
  }

  if (state.connection.reconnect_alarm) {
    state.connection.reconnect_alarm->Cancel();
  }
  state.connection.reconnect_delay = kInitialReconnectDelay;
  // Bump generation before destroying the old agent so its session-ended
  // callback is treated as stale and does not schedule a reconnect retry.
  const uint64_t agent_generation = ++state.connection.cast_generation;
  LOGI("TaskRunner: stopping old agent");
  // Properly destroy the old agent. Its encoder destructor joins
  // the encode thread, so no more tasks will be posted after this.
  state.connection.cast.reset();

  LOGI("TaskRunner: creating new agent");
  auto trust_store = openscreen::cast::CastTrustStore::Create();
  state.connection.cast =
      std::make_unique<openscreen::cast::ControllableFileCastAgent>(
          *state.task_runner, std::move(trust_store),
          [&state, agent_generation](const std::string& reason) {
            LOGI("Agent session ended: %s", reason.c_str());
            bool should_retry = false;
            std::string target;
            std::string video_path;
            std::chrono::milliseconds retry_delay = kInitialReconnectDelay;
            {
              std::lock_guard<std::mutex> lock(state.mutex);
              state.connection.connected = false;
              state.last_session_end_reason = reason;
              const bool stale_agent =
                  agent_generation != state.connection.cast_generation;
              UpdateStatusLocked(state);
              should_retry = !stale_agent &&
                             state.connection.reconnect_enabled &&
                             !state.connection.target.empty() &&
                             !state.video_path.empty();
              target = state.connection.target;
              video_path = state.video_path;
              retry_delay = state.connection.reconnect_delay;
              state.connection.reconnect_delay =
                  std::min(state.connection.reconnect_delay * 2,
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               kMaxReconnectDelay));
            }
            if (should_retry && state.connection.reconnect_alarm) {
              LOGI("Scheduling reconnect retry in %lld ms for target=%s file=%s",
                   static_cast<long long>(retry_delay.count()), target.c_str(),
                   video_path.c_str());
              state.connection.reconnect_alarm->ScheduleFromNow(
                  [&state]() {
                    RequestCastSessionRestart(state, "reconnect_alarm");
                  }, retry_delay);
            }
          });

  openscreen::cast::VideoViewport vp;
  vp.zoom = std::clamp<float>(zoom, 1.0f, 8.0f);
  vp.center_x = 0.5 - offset_x / 1920.0;
  vp.center_y = 0.5 - offset_y / 1080.0;
  state.connection.cast->SetViewport(vp);
  state.connection.cast->SeekTo(std::chrono::milliseconds(position_ms));
  if (playing) {
    state.connection.cast->Play();
  } else {
    state.connection.cast->Pause();
  }
  LOGI("TaskRunner: seeded agent state pos=%lld playing=%d zoom=%.3f offset=(%.1f,%.1f)",
       position_ms, playing ? 1 : 0, zoom, offset_x, offset_y);

  openscreen::cast::ConnectionSettings settings;
  settings.receiver_endpoint = endpoint;
  settings.path_to_file = video_path;
  settings.should_include_video = true;
  settings.use_android_rtp_hack = true;
  settings.use_remoting = false;
  settings.should_loop_video = false;
  settings.enable_dscp = true;
#if defined(CAST_STANDALONE_SENDER_HAVE_MEDIACODEC)
  if (state.use_hw_encode) {
    settings.codec = openscreen::cast::VideoCodec::kH264;
    settings.max_bitrate = 5000000;
    LOGI("Using hardware H.264 encoding");
  } else
#endif
  {
    settings.codec = openscreen::cast::VideoCodec::kVp8;
    settings.max_bitrate = 1500000;
    LOGI("Using software VP8 encoding");
  }

  settings.av_sync_offset =
      std::chrono::milliseconds(state.av_sync_offset_ms);
  settings.playout_delay =
      std::chrono::milliseconds(state.playout_delay_ms);
  settings.connect_timeout =
      std::chrono::milliseconds(state.connect_timeout_ms);
  settings.brightness = std::clamp(state.brightness, -200, 200);
  settings.enable_video_passthrough = state.enable_video_passthrough;

  LOGI("TaskRunner: connecting buf=%dms avsync=%lldms timeout=%dms brightness=%d pt=%d file=%s",
       state.playout_delay_ms,
       (long long)state.av_sync_offset_ms,
       state.connect_timeout_ms,
       state.brightness,
       state.enable_video_passthrough ? 1 : 0,
       video_path.c_str());
  state.connection.cast->Connect(std::move(settings));
  LOGI("TaskRunner: connect initiated");
}

void RequestCastSessionRestart(ControllerState& state, const char* reason) {
  std::string target;
  std::string video_path;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    target = state.connection.target;
    video_path = state.video_path;
    if (target.empty() || video_path.empty()) {
      return;
    }
    ++state.connection.desired_session_generation;
    if (state.connection.session_restart_posted) {
      LOGI("RequestCastSessionRestart[%s]: coalesced target=%s file=%s gen=%llu",
           reason ? reason : "unknown", target.c_str(), video_path.c_str(),
           static_cast<unsigned long long>(
               state.connection.desired_session_generation));
      return;
    }
    state.connection.session_restart_posted = true;
  }

  const std::string restart_reason = reason ? reason : "unknown";
  state.task_runner->PostTask([&state, restart_reason]() {
    while (true) {
      std::string current_target;
      std::string current_video_path;
      uint64_t generation = 0;
      {
        std::lock_guard<std::mutex> lock(state.mutex);
        current_target = state.connection.target;
        current_video_path = state.video_path;
        generation = state.connection.desired_session_generation;
      }

      LOGI("RequestCastSessionRestart[%s]: applying target=%s file=%s gen=%llu",
           restart_reason.c_str(), current_target.c_str(),
           current_video_path.c_str(),
           static_cast<unsigned long long>(generation));
      auto endpoint = ParseTarget(current_target);
      if (!endpoint.port) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.status = "Invalid target: " + current_target;
        state.connection.session_restart_posted = false;
        return;
      }
      StartCastSessionOnTaskRunner(state, endpoint, current_video_path);

      bool needs_another_pass = false;
      {
        std::lock_guard<std::mutex> lock(state.mutex);
        needs_another_pass =
            generation != state.connection.desired_session_generation;
        if (!needs_another_pass) {
          state.connection.session_restart_posted = false;
        }
      }
      if (!needs_another_pass) {
        return;
      }
      LOGI("RequestCastSessionRestart[%s]: detected newer desired session, retrying",
           restart_reason.c_str());
    }
  });
}

long long GetAgentPositionMsForLog(ControllerState& state) {
  if (!state.connection.cast) {
    return -1;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             state.connection.cast->GetCurrentPosition())
      .count();
}

void DrainPausedSeekQueue(ControllerState& state) {
  while (true) {
    long long pos = -1;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      if (state.connection.paused_seek_queue.empty()) {
        state.connection.paused_seek_drain_posted = false;
        return;
      }
      pos = state.connection.paused_seek_queue.front();
      state.connection.paused_seek_queue.pop_front();
    }

    if (!state.connection.cast) {
      continue;
    }

    LOGI("nativeSeekTo exec(paused-queue): target=%lld agent_pos_before=%lld",
         pos, GetAgentPositionMsForLog(state));
    state.connection.cast->SeekTo(std::chrono::milliseconds(pos));
    LOGI("nativeSeekTo exec(paused-queue): target=%lld agent_pos_after=%lld",
         pos, GetAgentPositionMsForLog(state));
  }
}
#endif  // HAVE_OPENSCREEN

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeInit(
    JNIEnv* env,
    jobject thiz) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
#ifdef HAVE_OPENSCREEN
  if (state.task_runner) {
    // Already initialized (activity was recreated).
    UpdateStatusLocked(state);
    return;
  }
  LOGI("nativeInit: starting task runner");
  openscreen::SetLogLevel(openscreen::LogLevel::kInfo);

  // Redirect stderr to logcat so OSP_LOG_* output is visible.
  {
    static int pfd[2];
    if (pipe(pfd) == 0) {
      dup2(pfd[1], STDERR_FILENO);
      close(pfd[1]);
      static std::thread log_thread([]() {
        char buf[512];
        ssize_t n;
        while ((n = read(pfd[0], buf, sizeof(buf) - 1)) > 0) {
          buf[n] = '\0';
          // Remove trailing newlines for cleaner logcat output.
          while (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';
          if (n > 0) {
            __android_log_print(ANDROID_LOG_INFO, "OSP", "%s", buf);
          }
        }
      });
      log_thread.detach();
    }
  }

  EnsureTaskRunner(state);
#endif
  UpdateStatusLocked(state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeTestCast(
    JNIEnv* env,
    jobject thiz,
    jstring target,
    jstring file_path) {
  auto& state = State();
  std::string target_str = JStringToStdString(env, target);
  std::string path = JStringToStdString(env, file_path);
  LOGI("nativeTestCast: target=%s file=%s", target_str.c_str(), path.c_str());

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.connection.target = target_str;
    state.video_path = path;
    state.video_uri = path;
    state.connection.reconnect_enabled = true;
  }

#ifdef HAVE_OPENSCREEN
  EnsureTaskRunner(state);
  RequestCastSessionRestart(state, "nativeTestCast");
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeConnect(
    JNIEnv* env,
    jobject thiz,
    jstring target) {
  auto& state = State();
  std::string target_str;
  std::string video_path;

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.connection.target = JStringToStdString(env, target);
    target_str = state.connection.target;
    video_path = state.video_path;
    state.connection.reconnect_enabled = true;
  }

#ifdef HAVE_OPENSCREEN
  if (!target_str.empty() && !video_path.empty()) {
    EnsureTaskRunner(state);
    RequestCastSessionRestart(state, "nativeConnect");
  } else {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.connection.connected = !target_str.empty();
    UpdateStatusLocked(state);
  }
#else
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.connection.connected = !target_str.empty();
    UpdateStatusLocked(state);
  }
#endif

  return !target_str.empty() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeDisconnect(
    JNIEnv* env,
    jobject thiz) {
  auto& state = State();
#ifdef HAVE_OPENSCREEN
  // Destroy agent on the task runner thread.
  if (state.task_runner) {
    state.task_runner->PostTask([&state]() {
      if (state.connection.reconnect_alarm) {
        state.connection.reconnect_alarm->Cancel();
      }
      {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.connection.paused_seek_queue.clear();
        state.connection.paused_seek_drain_posted = false;
      }
      state.connection.cast.reset();
    });
  }
#endif
  std::lock_guard<std::mutex> lock(state.mutex);
      state.connection.connected = false;
      state.active_mode = "idle";
      state.debug_state.clear();
      state.playing = false;
      state.desired_playing = false;
  state.connection.reconnect_enabled = false;
  state.connection.reconnect_delay = kInitialReconnectDelay;
  UpdateStatusLocked(state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeOpenVideo(
    JNIEnv* env,
    jobject thiz,
    jstring uri,
    jint fd1,
    jint fd2,
    jboolean mirror_locally,
    jlong start_position_ms,
    jboolean start_playing) {
  auto& state = State();
  std::string target_str;
  std::string video_path;

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.video_uri = JStringToStdString(env, uri);
    state.mirror_locally = mirror_locally == JNI_TRUE;
    state.position_ms = std::max<long long>(0, start_position_ms);
    state.pending_open_position_ms = state.position_ms;
    state.has_pending_open_position = true;
    state.connection.reconnect_enabled = true;

    if (fd1 >= 0 && fd2 >= 0) {
      // Dup both fds (Java's PFDs own the originals).
      if (state.video_fd >= 0) close(state.video_fd);
      if (state.video_fd2 >= 0) close(state.video_fd2);
      state.video_fd = dup(fd1);
      state.video_fd2 = dup(fd2);
      // Two independent fds for audio and video capturers.
      // Format: "fd:<fd1>,<fd2>" — first open uses fd1, second uses fd2.
      state.video_path = "fd:" + std::to_string(state.video_fd) +
                         "," + std::to_string(state.video_fd2);
      LOGI("nativeOpenVideo: fd1=%d fd2=%d path=%s", state.video_fd,
           state.video_fd2, state.video_path.c_str());
    }

    target_str = state.connection.target;
    video_path = state.video_path;
    state.playing = start_playing == JNI_TRUE;
    state.desired_playing = start_playing == JNI_TRUE;
    LOGI("nativeOpenVideo: start_pos=%lld start_playing=%d",
         state.position_ms, state.desired_playing ? 1 : 0);
  }

#ifdef HAVE_OPENSCREEN
  if (!target_str.empty() && !video_path.empty()) {
    EnsureTaskRunner(state);
    RequestCastSessionRestart(state, "nativeOpenVideo");
  }
#endif

  std::lock_guard<std::mutex> lock(state.mutex);
  UpdateStatusLocked(state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeOpenVideoPath(
    JNIEnv* env,
    jobject thiz,
    jstring uri,
    jstring file_path,
    jboolean mirror_locally,
    jlong start_position_ms,
    jboolean start_playing) {
  auto& state = State();
  std::string target_str;
  std::string video_path;

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.video_uri = JStringToStdString(env, uri);
    state.mirror_locally = mirror_locally == JNI_TRUE;
    state.position_ms = std::max<long long>(0, start_position_ms);
    state.pending_open_position_ms = state.position_ms;
    state.has_pending_open_position = true;
    state.video_path = JStringToStdString(env, file_path);
    state.connection.reconnect_enabled = true;
    target_str = state.connection.target;
    video_path = state.video_path;
    state.playing = start_playing == JNI_TRUE;
    state.desired_playing = start_playing == JNI_TRUE;
    LOGI("nativeOpenVideoPath: path=%s target=%s start_pos=%lld start_playing=%d",
         video_path.c_str(), target_str.c_str(), state.position_ms,
         state.desired_playing ? 1 : 0);
  }

#ifdef HAVE_OPENSCREEN
  if (!target_str.empty() && !video_path.empty()) {
    EnsureTaskRunner(state);
    RequestCastSessionRestart(state, "nativeOpenVideoPath");
  }
#endif

  std::lock_guard<std::mutex> lock(state.mutex);
  UpdateStatusLocked(state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativePlay(
    JNIEnv* env,
    jobject thiz) {
  auto& state = State();
  LOGI("nativePlay request: connected=%d playing=%d state_pos=%lld uri=%s",
       state.connection.connected, state.playing, state.position_ms,
       state.video_uri.c_str());
#ifdef HAVE_OPENSCREEN
  if (state.connection.cast && state.task_runner) {
    state.task_runner->PostTask([&state]() {
      if (state.connection.cast) {
        {
          std::lock_guard<std::mutex> lock(state.mutex);
          state.connection.paused_seek_queue.clear();
          state.connection.paused_seek_drain_posted = false;
        }
        LOGI("nativePlay exec: agent_pos_before=%lld",
             GetAgentPositionMsForLog(state));
        state.connection.cast->Play();
        LOGI("nativePlay exec: agent_pos_after=%lld",
             GetAgentPositionMsForLog(state));
      }
    });
  }
#endif
  std::lock_guard<std::mutex> lock(state.mutex);
  state.playing = state.connection.connected && !state.video_uri.empty();
  state.desired_playing = state.connection.connected && !state.video_uri.empty();
  UpdateStatusLocked(state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativePause(
    JNIEnv* env,
    jobject thiz) {
  auto& state = State();
  LOGI("nativePause request: connected=%d playing=%d state_pos=%lld uri=%s",
       state.connection.connected, state.playing, state.position_ms,
       state.video_uri.c_str());
#ifdef HAVE_OPENSCREEN
  if (state.connection.cast && state.task_runner) {
    state.task_runner->PostTask([&state]() {
      if (state.connection.cast) {
        LOGI("nativePause exec: agent_pos_before=%lld",
             GetAgentPositionMsForLog(state));
        state.connection.cast->Pause();
        LOGI("nativePause exec: agent_pos_after=%lld",
             GetAgentPositionMsForLog(state));
      }
    });
  }
#endif
  std::lock_guard<std::mutex> lock(state.mutex);
  state.playing = false;
  state.desired_playing = false;
  UpdateStatusLocked(state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeSeekTo(
    JNIEnv* env,
    jobject thiz,
    jlong position_ms) {
  auto& state = State();
  long long pos = std::max<long long>(0, position_ms);
  bool should_restart_media = false;
  LOGI("nativeSeekTo request: pos=%lld connected=%d playing=%d state_pos=%lld",
       pos, state.connection.connected, state.playing, state.position_ms);
#ifdef HAVE_OPENSCREEN
  if (state.connection.cast && state.task_runner) {
    bool queue_paused_seek = false;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      queue_paused_seek = !state.playing;
      const auto now = std::chrono::steady_clock::now();
      if (state.connection.rapid_seek_count > 0 &&
          now - state.connection.last_seek_at <= kRapidSeekGap) {
        ++state.connection.rapid_seek_count;
      } else {
        state.connection.rapid_seek_count = 1;
      }
      state.connection.last_seek_at = now;
      should_restart_media =
          state.connection.rapid_seek_count >= kSeekStormThreshold &&
          !state.connection.target.empty() &&
          !state.video_path.empty() &&
          (state.connection.last_seek_restart_at.time_since_epoch().count() ==
               0 ||
           now - state.connection.last_seek_restart_at >=
               kSeekStormRestartCooldown);
      if (should_restart_media) {
        state.connection.last_seek_restart_at = now;
        state.connection.rapid_seek_count = 0;
        state.connection.paused_seek_queue.clear();
        state.connection.paused_seek_drain_posted = false;
        state.desired_playing = true;
        state.playing = true;
        LOGI("nativeSeekTo storm: forcing in-session media restart at pos=%lld",
             pos);
      }
      if (queue_paused_seek) {
        if (state.connection.paused_seek_queue.size() >=
            kPausedSeekQueueCapacity) {
          const auto dropped = state.connection.paused_seek_queue.front();
          state.connection.paused_seek_queue.pop_front();
          LOGI("nativeSeekTo queue drop_oldest: dropped=%lld capacity=%zu",
               dropped, kPausedSeekQueueCapacity);
        }
        state.connection.paused_seek_queue.push_back(pos);
        LOGI("nativeSeekTo queue push: target=%lld size=%zu/%zu",
             pos, state.connection.paused_seek_queue.size(),
             kPausedSeekQueueCapacity);
        if (!state.connection.paused_seek_drain_posted) {
          state.connection.paused_seek_drain_posted = true;
          state.task_runner->PostTask([&state]() { DrainPausedSeekQueue(state); });
        }
      }
    }
    if (should_restart_media) {
      state.task_runner->PostTask([&state, pos]() {
        if (state.connection.cast) {
          LOGI("nativeSeekTo storm exec: target=%lld agent_pos_before=%lld",
               pos, GetAgentPositionMsForLog(state));
          state.connection.cast->RecoverFromSeekStorm(
              std::chrono::milliseconds(pos), true);
          LOGI("nativeSeekTo storm exec: target=%lld agent_pos_after=%lld",
               pos, GetAgentPositionMsForLog(state));
        }
      });
    } else if (!queue_paused_seek) {
      state.task_runner->PostTask([&state, pos]() {
        if (state.connection.cast) {
          LOGI("nativeSeekTo exec: target=%lld agent_pos_before=%lld",
               pos, GetAgentPositionMsForLog(state));
          state.connection.cast->SeekTo(std::chrono::milliseconds(pos));
          LOGI("nativeSeekTo exec: target=%lld agent_pos_after=%lld",
               pos, GetAgentPositionMsForLog(state));
        }
      });
    }
  }
#endif
  std::lock_guard<std::mutex> lock(state.mutex);
  state.position_ms = pos;
  UpdateStatusLocked(state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeUpdateViewport(
    JNIEnv* env,
    jobject thiz,
    jfloat zoom,
    jfloat offset_x,
    jfloat offset_y) {
  auto& state = State();
  float z = std::clamp<float>(zoom, 1.0f, 8.0f);
#ifdef HAVE_OPENSCREEN
  if (state.connection.cast && state.task_runner) {
    openscreen::cast::VideoViewport vp;
    vp.zoom = z;
    vp.center_x = 0.5 - offset_x / 1920.0;
    vp.center_y = 0.5 - offset_y / 1080.0;
    state.task_runner->PostTask([&state, vp]() {
      if (state.connection.cast) state.connection.cast->SetViewport(vp);
    });
  }
#endif
  std::lock_guard<std::mutex> lock(state.mutex);
  state.zoom = z;
  state.offset_x = offset_x;
  state.offset_y = offset_y;
  UpdateStatusLocked(state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeSetMirrorLocally(
    JNIEnv* env,
    jobject thiz,
    jboolean enabled) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.mirror_locally = enabled == JNI_TRUE;
  UpdateStatusLocked(state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeSetHwEncode(
    JNIEnv* env,
    jobject thiz,
    jboolean enabled) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.use_hw_encode = enabled == JNI_TRUE;
  LOGI("Hardware encoding %s", state.use_hw_encode ? "enabled" : "disabled");
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeSetVideoPassthroughEnabled(
    JNIEnv* env,
    jobject thiz,
    jboolean enabled) {
  auto& state = State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.enable_video_passthrough = enabled == JNI_TRUE;
    UpdateStatusLocked(state);
  }
  LOGI("Video passthrough %s",
       state.enable_video_passthrough ? "enabled" : "disabled");
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeSetBrightness(
    JNIEnv* env,
    jobject thiz,
    jint brightness) {
  auto& state = State();
  const int clamped = std::clamp(static_cast<int>(brightness), -200, 200);
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.brightness = clamped;
  }
  LOGI("Brightness set to %d", clamped);
#ifdef HAVE_OPENSCREEN
  if (state.connection.cast && state.task_runner) {
    state.task_runner->PostTask([&state, clamped]() {
      if (state.connection.cast) {
        state.connection.cast->SetBrightness(clamped);
      }
    });
  }
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeSetPlayoutDelay(
    JNIEnv* env,
    jobject thiz,
    jint delay_ms) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.playout_delay_ms = std::max(100, (int)delay_ms);
  LOGI("Playout delay set to %d ms (takes effect on next session)",
       state.playout_delay_ms);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeSetConnectTimeoutMs(
    JNIEnv* env,
    jobject thiz,
    jint timeout_ms) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.connect_timeout_ms = std::max(1000, static_cast<int>(timeout_ms));
  LOGI("Connect timeout set to %d ms (takes effect on next session)",
       state.connect_timeout_ms);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeSetAvSyncOffset(
    JNIEnv* env,
    jobject thiz,
    jlong offset_ms) {
  auto& state = State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.av_sync_offset_ms = offset_ms;
  }
  LOGI("A/V sync offset set to %lld ms", (long long)offset_ms);
#ifdef HAVE_OPENSCREEN
  if (state.connection.cast && state.task_runner) {
    auto dur = std::chrono::milliseconds(offset_ms);
    state.task_runner->PostTask([&state, dur]() {
      if (state.connection.cast) state.connection.cast->SetAvSyncOffset(dur);
    });
  }
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeIsConnected(
    JNIEnv* env,
    jobject thiz) {
  auto& state = State();
#ifdef HAVE_OPENSCREEN
  if (state.connection.cast) {
    return state.connection.cast->IsConnected() ? JNI_TRUE : JNI_FALSE;
  }
  return JNI_FALSE;
#else
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.connection.connected ? JNI_TRUE : JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeIsPlaying(
    JNIEnv* env,
    jobject thiz) {
  auto& state = State();
#ifdef HAVE_OPENSCREEN
  if (state.connection.cast) {
    return (state.connection.cast->IsConnected() &&
            state.connection.cast->IsPlaying())
               ? JNI_TRUE
               : JNI_FALSE;
  }
  return JNI_FALSE;
#else
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.playing ? JNI_TRUE : JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeGetDurationMs(
    JNIEnv* env,
    jobject thiz) {
  auto& state = State();
#ifdef HAVE_OPENSCREEN
  if (state.connection.cast) {
    auto dur = state.connection.cast->GetDuration();
    return std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
  }
#endif
  return 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeGetPositionMs(
    JNIEnv* env,
    jobject thiz) {
  auto& state = State();
#ifdef HAVE_OPENSCREEN
  if (state.connection.cast) {
    auto pos = state.connection.cast->GetCurrentPosition();
    return std::chrono::duration_cast<std::chrono::milliseconds>(pos).count();
  }
#endif
  return state.position_ms;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeGetStatus(
    JNIEnv* env,
    jobject thiz) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
#ifdef HAVE_OPENSCREEN
  if (state.connection.cast) {
    state.connection.connected = state.connection.cast->IsConnected();
    state.playing = state.connection.connected && state.connection.cast->IsPlaying();
    if (state.has_pending_open_position) {
      state.position_ms = state.pending_open_position_ms;
    } else {
      state.position_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              state.connection.cast->GetCurrentPosition())
                              .count();
    }
    state.active_mode = state.connection.cast->GetActiveModeString();
    state.debug_state = state.connection.cast->GetDebugStateString();
    UpdateStatusLocked(state);
  }
#endif
  return StdStringToJString(env, state.status);
}
