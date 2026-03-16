// JNI bridge between the Android ControlCast app and the Open Screen
// ControllableFileCastAgent.  When built with the full Open Screen library
// (HAVE_OPENSCREEN defined), native connect/play/pause/seek/viewport calls
// are forwarded to the real Cast sender.  Otherwise a status-only stub is
// compiled so the APK can still be built and run for UI development.

#include <jni.h>
#include <unistd.h>

#include <android/log.h>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

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
#include "util/chrono_helpers.h"
#endif  // HAVE_OPENSCREEN

namespace {

struct ControllerState {
  std::mutex mutex;
  std::string target;
  std::string video_uri;
  bool connected = false;
  bool mirror_locally = true;
  bool playing = false;
  long long position_ms = 0;
  float zoom = 1.0f;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  int video_fd = -1;
  int video_fd2 = -1;
  std::string video_path;
  bool use_hw_encode = true;
  long long av_sync_offset_ms = 0;
  int playout_delay_ms = 400;
  std::string status = "Native backend ready.";

#ifdef HAVE_OPENSCREEN
  openscreen::TaskRunnerImpl* task_runner = nullptr;
  std::unique_ptr<openscreen::cast::ControllableFileCastAgent> agent;
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
  if (!state.connected) {
    stream << "Not connected.";
    if (!state.target.empty()) {
      stream << " Target: " << state.target;
    }
    state.status = stream.str();
    return;
  }

  stream << "Connected to " << state.target;
  if (!state.video_uri.empty()) {
    stream << " | " << (state.playing ? "Playing" : "Paused");
    stream << " @ " << state.position_ms / 1000.0 << "s";
    stream << " | zoom " << state.zoom;
    stream << " | local mirror " << (state.mirror_locally ? "on" : "off");
  } else {
    stream << " | no video selected";
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
  }
}

// (Re)create the agent and connect to the Cast receiver.
// Posts the work to the task runner thread because the agent and its
// TLS factory must be created and destroyed on that thread.
void StartCastSession(ControllerState& state,
                      const std::string& target,
                      const std::string& video_path) {
  auto endpoint = ParseTarget(target);
  if (!endpoint.port) {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.status = "Invalid target: " + target;
    return;
  }

  LOGI("StartCastSession: target=%s file=%s", target.c_str(), video_path.c_str());

  state.task_runner->PostTask(
      [&state, endpoint, video_path]() {
        LOGI("TaskRunner: stopping old agent");
        // Properly destroy the old agent. Its encoder destructor joins
        // the encode thread, so no more tasks will be posted after this.
        state.agent.reset();

        LOGI("TaskRunner: creating new agent");
        auto trust_store = openscreen::cast::CastTrustStore::Create();
        state.agent =
            std::make_unique<openscreen::cast::ControllableFileCastAgent>(
                *state.task_runner, std::move(trust_store),
                [&state]() {
                  LOGI("Agent session ended");
                  std::lock_guard<std::mutex> lock(state.mutex);
                  state.connected = false;
                  UpdateStatusLocked(state);
                });

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

        LOGI("TaskRunner: connecting buf=%dms avsync=%lldms file=%s",
             state.playout_delay_ms,
             (long long)state.av_sync_offset_ms,
             video_path.c_str());
        state.agent->Connect(std::move(settings));

        {
          std::lock_guard<std::mutex> lock(state.mutex);
          state.connected = true;
          UpdateStatusLocked(state);
        }
        LOGI("TaskRunner: connect initiated");
      });
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
    state.target = target_str;
    state.video_path = path;
    state.video_uri = path;
  }

#ifdef HAVE_OPENSCREEN
  EnsureTaskRunner(state);
  StartCastSession(state, target_str, path);
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
    state.target = JStringToStdString(env, target);
    target_str = state.target;
    video_path = state.video_path;
  }

#ifdef HAVE_OPENSCREEN
  if (!target_str.empty() && !video_path.empty()) {
    StartCastSession(state, target_str, video_path);
  } else {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.connected = !target_str.empty();
    UpdateStatusLocked(state);
  }
#else
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.connected = !target_str.empty();
    UpdateStatusLocked(state);
  }
#endif

  std::lock_guard<std::mutex> lock(state.mutex);
  return state.connected ? JNI_TRUE : JNI_FALSE;
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
      state.agent.reset();
    });
  }
#endif
  std::lock_guard<std::mutex> lock(state.mutex);
  state.connected = false;
  state.playing = false;
  UpdateStatusLocked(state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeOpenVideo(
    JNIEnv* env,
    jobject thiz,
    jstring uri,
    jint fd1,
    jint fd2,
    jboolean mirror_locally) {
  auto& state = State();
  std::string target_str;
  std::string video_path;

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.video_uri = JStringToStdString(env, uri);
    state.mirror_locally = mirror_locally == JNI_TRUE;
    state.position_ms = 0;

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

    target_str = state.target;
    video_path = state.video_path;
    state.playing = state.connected;
  }

#ifdef HAVE_OPENSCREEN
  if (!target_str.empty() && !video_path.empty()) {
    StartCastSession(state, target_str, video_path);
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
    jboolean mirror_locally) {
  auto& state = State();
  std::string target_str;
  std::string video_path;

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.video_uri = JStringToStdString(env, uri);
    state.mirror_locally = mirror_locally == JNI_TRUE;
    state.position_ms = 0;
    state.video_path = JStringToStdString(env, file_path);
    target_str = state.target;
    video_path = state.video_path;
    state.playing = state.connected;
    LOGI("nativeOpenVideoPath: path=%s target=%s", video_path.c_str(),
         target_str.c_str());
  }

#ifdef HAVE_OPENSCREEN
  if (!target_str.empty() && !video_path.empty()) {
    StartCastSession(state, target_str, video_path);
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
#ifdef HAVE_OPENSCREEN
  if (state.agent && state.task_runner) {
    state.task_runner->PostTask([&state]() {
      if (state.agent) state.agent->Play();
    });
  }
#endif
  std::lock_guard<std::mutex> lock(state.mutex);
  state.playing = state.connected && !state.video_uri.empty();
  UpdateStatusLocked(state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativePause(
    JNIEnv* env,
    jobject thiz) {
  auto& state = State();
#ifdef HAVE_OPENSCREEN
  if (state.agent && state.task_runner) {
    state.task_runner->PostTask([&state]() {
      if (state.agent) state.agent->Pause();
    });
  }
#endif
  std::lock_guard<std::mutex> lock(state.mutex);
  state.playing = false;
  UpdateStatusLocked(state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeSeekTo(
    JNIEnv* env,
    jobject thiz,
    jlong position_ms) {
  auto& state = State();
  long long pos = std::max<long long>(0, position_ms);
#ifdef HAVE_OPENSCREEN
  if (state.agent && state.task_runner) {
    state.task_runner->PostTask([&state, pos]() {
      if (state.agent) {
        state.agent->SeekTo(std::chrono::milliseconds(pos));
      }
    });
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
  if (state.agent && state.task_runner) {
    openscreen::cast::VideoViewport vp;
    vp.zoom = z;
    vp.center_x = 0.5 - offset_x / 1920.0;
    vp.center_y = 0.5 - offset_y / 1080.0;
    state.task_runner->PostTask([&state, vp]() {
      if (state.agent) state.agent->SetViewport(vp);
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
  if (state.agent && state.task_runner) {
    auto dur = std::chrono::milliseconds(offset_ms);
    state.task_runner->PostTask([&state, dur]() {
      if (state.agent) state.agent->SetAvSyncOffset(dur);
    });
  }
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeIsPlaying(
    JNIEnv* env,
    jobject thiz) {
  auto& state = State();
#ifdef HAVE_OPENSCREEN
  if (state.agent) {
    return state.agent->IsPlaying() ? JNI_TRUE : JNI_FALSE;
  }
#endif
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.playing ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_openscreen_controlcast_NativeBackedBackend_nativeGetPositionMs(
    JNIEnv* env,
    jobject thiz) {
  auto& state = State();
#ifdef HAVE_OPENSCREEN
  if (state.agent) {
    // TODO: this reads from the sender on the JNI thread; safe because
    // GetCurrentPosition only reads atomic/const fields.
    auto pos = state.agent->GetCurrentPosition();
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
  return StdStringToJString(env, state.status);
}
