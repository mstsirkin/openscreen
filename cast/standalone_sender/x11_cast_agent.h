// X11 Cast Agent - manages the Cast session for screen mirroring.
// Based on LoopingFileCastAgent but uses X11Sender instead of LoopingFileSender.

#ifndef CAST_STANDALONE_SENDER_X11_CAST_AGENT_H_
#define CAST_STANDALONE_SENDER_X11_CAST_AGENT_H_

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
#include "cast/standalone_sender/x11_sender.h"
#include "cast/streaming/public/environment.h"
#include "cast/streaming/public/sender_session.h"
#include "platform/base/error.h"
#include "platform/impl/task_runner.h"
#include "util/scoped_wake_lock.h"

namespace Json {
class Value;
}

namespace openscreen::cast {

// Manages the Cast session workflow for X11 screen mirroring.
// Same flow as LoopingFileCastAgent: TLS connect -> LAUNCH mirroring app
// -> OFFER/ANSWER -> stream, but uses X11Sender for live capture.
class X11CastAgent final
    : public SenderSocketFactory::Client,
      public VirtualConnectionRouter::SocketErrorHandler,
      public ConnectionNamespaceHandler::VirtualConnectionPolicy,
      public CastMessageHandler,
      public SenderSession::Client,
      public SenderStatsClient {
 public:
  using ShutdownCallback = std::function<void()>;

  X11CastAgent(TaskRunner& task_runner,
               std::unique_ptr<TrustStore> cast_trust_store,
               ShutdownCallback shutdown_callback);
  ~X11CastAgent();

  void Connect(ConnectionSettings settings, unsigned long window_id = 0);

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

  const char* GetStreamingAppId() const;
  void HandleReceiverStatus(const Json::Value& status);
  void OnRemoteMessagingOpened(bool success);
  void OnReceiverMessagingOpened(bool success);
  void CreateAndStartSession();

  void OnNegotiated(const SenderSession* session,
                    SenderSession::ConfiguredSenders senders,
                    capture_recommendations::Recommendations
                        capture_recommendations) override;
  void OnError(const SenderSession* session, const Error& error) override;

  void OnStatisticsUpdated(const SenderStats& updated_stats) override;

  void StartX11Sender();
  void Shutdown();

  TaskRunner& task_runner_;
  ShutdownCallback shutdown_callback_;
  VirtualConnectionRouter router_;
  ConnectionNamespaceHandler connection_handler_;
  SenderSocketFactory socket_factory_;
  std::unique_ptr<TlsConnectionFactory> connection_factory_;
  CastSocketMessagePort message_port_;

  int next_request_id_ = 1;

  std::optional<ConnectionSettings> connection_settings_;
  ScopedWakeLockPtr wake_lock_;
  std::string app_session_id_;

  std::optional<VirtualConnection> remote_connection_;
  std::optional<VirtualConnection> platform_remote_connection_;

  std::unique_ptr<Environment> environment_;
  std::unique_ptr<SenderSession> current_session_;
  std::unique_ptr<X11Sender> x11_sender_;
  std::unique_ptr<SenderSession::ConfiguredSenders> current_negotiation_;

  unsigned long window_id_ = 0;
  bool has_launched_ = false;
  bool shutting_down_ = false;
  int num_stats_updates_ = 0;
  std::optional<SenderStats> last_stats_;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_X11_CAST_AGENT_H_
