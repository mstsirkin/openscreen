// Copyright 2026

#ifndef CAST_STANDALONE_SENDER_CONTROLLABLE_FILE_CAST_AGENT_H_
#define CAST_STANDALONE_SENDER_CONTROLLABLE_FILE_CAST_AGENT_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "cast/common/channel/cast_message_handler.h"
#include "cast/common/channel/cast_socket_message_port.h"
#include "cast/common/channel/connection_namespace_handler.h"
#include "cast/common/channel/virtual_connection_router.h"
#include "cast/common/public/cast_socket.h"
#include "cast/common/public/trust_store.h"
#include "cast/sender/public/sender_socket_factory.h"
#include "cast/standalone_sender/connection_settings.h"
#include "cast/standalone_sender/controllable_file_sender.h"
#include "cast/streaming/public/environment.h"
#include "cast/streaming/public/sender_session.h"
#include "platform/api/tls_connection_factory.h"
#include "platform/base/error.h"
#include "platform/impl/task_runner.h"
#include "util/alarm.h"

namespace Json {
class Value;
}

namespace openscreen::cast {

class ControllableFileCastAgent final
    : public SenderSocketFactory::Client,
      public VirtualConnectionRouter::SocketErrorHandler,
      public ConnectionNamespaceHandler::VirtualConnectionPolicy,
      public CastMessageHandler,
      public SenderSession::Client,
      public SenderStatsClient {
 public:
  using ShutdownCallback = std::function<void(const std::string&)>;

  ControllableFileCastAgent(TaskRunner& task_runner,
                            std::unique_ptr<TrustStore> cast_trust_store,
                            ShutdownCallback shutdown_callback);
  ~ControllableFileCastAgent();

  void Connect(ConnectionSettings settings);

  void Play();
  void Pause();
  void SeekTo(Clock::duration position);
  void SeekBy(Clock::duration delta);
  void RecoverFromSeekStorm(Clock::duration position, bool resume_playback);
  void SetViewport(const VideoViewport& viewport);
  void ResetViewport();
  void SetAvSyncOffset(Clock::duration offset);
  void SetBrightness(int brightness);
  Clock::duration GetCurrentPosition() const;
  Clock::duration GetDuration() const;
  bool IsConnected() const;
  bool IsPlaying() const;
  std::string GetActiveModeString() const;
  std::string GetDebugStateString() const;

 private:
  void OnConnected(SenderSocketFactory* factory,
                   const IPEndpoint& endpoint,
                   std::unique_ptr<CastSocket> socket) override;
  void OnError(SenderSocketFactory* factory,
               const IPEndpoint& endpoint,
               const Error& error) override;

  void OnClose(CastSocket* cast_socket) override;
  void OnError(CastSocket* socket, const Error& error) override;

  bool IsConnectionAllowed(
      const VirtualConnection& virtual_conn) const override;

  void OnMessage(VirtualConnectionRouter* router,
                 CastSocket* socket,
                 proto::CastMessage message) override;

  void OnNegotiated(const SenderSession* session,
                    SenderSession::ConfiguredSenders senders,
                    capture_recommendations::Recommendations
                        capture_recommendations) override;
  void OnError(const SenderSession* session, const Error& error) override;
  void OnStatisticsUpdated(const SenderStats& updated_stats) override;

  const char* GetStreamingAppId() const;
  void HandleReceiverStatus(const Json::Value& status);
  void HandleMediaStatus(const Json::Value& status);
  void OnRemoteMessagingOpened(bool success);
  void OnReceiverMessagingOpened(bool success);
  void CreateAndStartSession();
  void StartSender();
  void Shutdown(const std::string& reason = "shutdown");
  bool HasFreshReceiverMediaStatus() const;

  TaskRunner& task_runner_;
  ShutdownCallback shutdown_callback_;
  VirtualConnectionRouter router_;
  ConnectionNamespaceHandler connection_handler_;
  SenderSocketFactory socket_factory_;
  std::unique_ptr<TlsConnectionFactory> connection_factory_;
  CastSocketMessagePort message_port_;
  Alarm connect_timeout_alarm_;

  int next_request_id_ = 1;
  std::optional<ConnectionSettings> connection_settings_;
  std::string app_session_id_;
  std::optional<VirtualConnection> remote_connection_;
  std::optional<VirtualConnection> platform_remote_connection_;

  std::unique_ptr<Environment> environment_;
  std::unique_ptr<SenderSession> current_session_;
  std::unique_ptr<ControllableFileSender> sender_;
  std::unique_ptr<SenderSession::ConfiguredSenders> current_negotiation_;

  Clock::duration desired_position_{};
  bool desired_paused_ = false;
  VideoViewport desired_viewport_;
  bool has_launched_ = false;
  bool shutting_down_ = false;
  std::string last_receiver_player_state_;
  std::string last_receiver_idle_reason_;
  std::string last_receiver_media_session_id_;
  double last_receiver_current_time_seconds_ = -1.0;
  Clock::time_point last_receiver_media_status_at_{};
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_CONTROLLABLE_FILE_CAST_AGENT_H_
