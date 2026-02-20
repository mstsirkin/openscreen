// X11 Cast Agent implementation.
// Nearly identical to LoopingFileCastAgent, but creates X11Sender
// instead of LoopingFileSender after session negotiation.

#include "cast/standalone_sender/x11_cast_agent.h"

#include <string>
#include <utility>

#include "cast/common/channel/message_util.h"
#include "cast/streaming/capture_configs.h"
#include "util/chrono_helpers.h"
#include "cast/common/public/cast_streaming_app_ids.h"
#include "cast/streaming/public/capture_recommendations.h"
#include "cast/streaming/public/constants.h"
#include "cast/streaming/public/offer_messages.h"
#include "json/value.h"
#include "platform/api/tls_connection_factory.h"
#include "util/json/json_helpers.h"
#include "util/stringprintf.h"
#include "util/trace_logging.h"

namespace openscreen::cast {

using DeviceMediaPolicy = SenderSocketFactory::DeviceMediaPolicy;

X11CastAgent::X11CastAgent(TaskRunner& task_runner,
                           std::unique_ptr<TrustStore> cast_trust_store,
                           ShutdownCallback shutdown_callback)
    : task_runner_(task_runner),
      shutdown_callback_(std::move(shutdown_callback)),
      connection_handler_(router_, *this),
      socket_factory_(*this, task_runner_, std::move(cast_trust_store),
                      CastCRLTrustStore::Create()),
      connection_factory_(
          TlsConnectionFactory::CreateFactory(socket_factory_, task_runner_)),
      message_port_(router_) {
  router_.AddHandlerForLocalId(kPlatformSenderId, this);
  socket_factory_.set_factory(connection_factory_.get());
}

X11CastAgent::~X11CastAgent() {
  Shutdown();
}

void X11CastAgent::Connect(ConnectionSettings settings, unsigned long window_id) {
  window_id_ = window_id;
  connection_settings_ = std::move(settings);
  task_runner_.PostTask([this] {
    socket_factory_.Connect(connection_settings_->receiver_endpoint,
                            DeviceMediaPolicy::kIncludesVideo, &router_);
  });
}

void X11CastAgent::OnConnected(SenderSocketFactory* factory,
                                const IPEndpoint& endpoint,
                                std::unique_ptr<CastSocket> socket) {
  if (message_port_.GetSocketId() != ToCastSocketId(nullptr)) {
    OSP_LOG_WARN << "Already connected, dropping: " << endpoint;
    return;
  }
  message_port_.SetSocket(socket->GetWeakPtr());
  router_.TakeSocket(this, std::move(socket));

  OSP_LOG_INFO << "Launching Mirroring App on Cast Receiver...";
  platform_remote_connection_.emplace(VirtualConnection{
      kPlatformSenderId, kPlatformReceiverId, message_port_.GetSocketId()});
  connection_handler_.OpenRemoteConnection(
      *platform_remote_connection_,
      [this](bool success) { OnReceiverMessagingOpened(success); });
}

void X11CastAgent::OnError(SenderSocketFactory* factory,
                            const IPEndpoint& endpoint,
                            const Error& error) {
  OSP_LOG_ERROR << "Socket factory error: " << error;
  Shutdown();
}

void X11CastAgent::OnClose(CastSocket* cast_socket) {
  OSP_VLOG << "Socket closed.";
  Shutdown();
}

void X11CastAgent::OnError(CastSocket* socket, const Error& error) {
  OSP_LOG_ERROR << "Socket error: " << error;
  Shutdown();
}

bool X11CastAgent::IsConnectionAllowed(
    const VirtualConnection& virtual_conn) const {
  return true;
}

void X11CastAgent::OnMessage(VirtualConnectionRouter* router,
                              CastSocket* socket,
                              proto::CastMessage message) {
  if (message_port_.GetSocketId() == ToCastSocketId(socket) &&
      !message_port_.source_id().empty() &&
      message_port_.source_id() == message.destination_id()) {
    message_port_.OnMessage(router, socket, std::move(message));
    return;
  }

  if (message.destination_id() != kPlatformSenderId &&
      message.destination_id() != kBroadcastId) {
    return;
  }

  if (message.namespace_() == kReceiverNamespace &&
      message_port_.GetSocketId() == ToCastSocketId(socket)) {
    const ErrorOr<Json::Value> payload = json::Parse(GetPayload(message));
    if (payload.is_error()) {
      return;
    }

    if (HasType(payload.value(), CastMessageType::kReceiverStatus)) {
      HandleReceiverStatus(payload.value());
    } else if (HasType(payload.value(), CastMessageType::kLaunchError)) {
      std::string reason;
      json::TryParseString(payload.value()[kMessageKeyReason], &reason);
      OSP_LOG_ERROR << "Launch error: " << reason;
      Shutdown();
    }
  }
}

const char* X11CastAgent::GetStreamingAppId() const {
  return GetCastStreamingAudioVideoAppId();
}

void X11CastAgent::HandleReceiverStatus(const Json::Value& status) {
  const Json::Value& details =
      (status[kMessageKeyStatus].isObject() &&
       status[kMessageKeyStatus][kMessageKeyApplications].isArray())
          ? status[kMessageKeyStatus][kMessageKeyApplications][0]
          : Json::Value();

  std::string running_app_id;
  if (!json::TryParseString(details[kMessageKeyAppId], &running_app_id) ||
      running_app_id != GetStreamingAppId()) {
    if (has_launched_) {
      Shutdown();
    }
    return;
  }

  has_launched_ = true;

  std::string session_id;
  if (!json::TryParseString(details[kMessageKeySessionId], &session_id) ||
      session_id.empty()) {
    Shutdown();
    return;
  }
  if (app_session_id_.empty()) {
    app_session_id_ = session_id;
  } else if (app_session_id_ != session_id) {
    Shutdown();
    return;
  }

  if (remote_connection_) {
    return;
  }

  std::string transport_id;
  if (!json::TryParseString(details[kMessageKeyTransportId], &transport_id) ||
      transport_id.empty()) {
    Shutdown();
    return;
  }

  remote_connection_.emplace(
      VirtualConnection{MakeUniqueSessionId("x11cast_sender"),
                        transport_id, message_port_.GetSocketId()});
  OSP_LOG_INFO << "Connecting to Mirroring App (session=" << app_session_id_
               << ")...";
  connection_handler_.OpenRemoteConnection(
      *remote_connection_,
      [this](bool success) { OnRemoteMessagingOpened(success); });
}

void X11CastAgent::OnRemoteMessagingOpened(bool success) {
  if (!remote_connection_) return;

  if (success) {
    OSP_LOG_INFO << "Starting streaming session...";
    CreateAndStartSession();
  } else {
    OSP_LOG_ERROR << "Failed to establish messaging to Mirroring App.";
    Shutdown();
  }
}

void X11CastAgent::OnReceiverMessagingOpened(bool success) {
  if (!success) {
    OSP_LOG_ERROR << "Failed to establish platform messaging.";
    Shutdown();
    return;
  }

  static constexpr char kLaunchTemplate[] =
      R"({{"type":"LAUNCH", "requestId":{}, "appId":"{}", "language": "en-US",
       "supportedAppTypes":["WEB"]}})";
  router_.Send(*platform_remote_connection_,
               MakeSimpleUTF8Message(
                   kReceiverNamespace,
                   StringFormat(kLaunchTemplate, next_request_id_++,
                                GetStreamingAppId())));
}

void X11CastAgent::CreateAndStartSession() {
  environment_ =
      std::make_unique<Environment>(&Clock::now, task_runner_, IPEndpoint{});

  SenderSession::Configuration config{
      connection_settings_->receiver_endpoint.address,
      *this,
      environment_.get(),
      &message_port_,
      remote_connection_->local_id,
      remote_connection_->peer_id,
      connection_settings_->use_android_rtp_hack,
      connection_settings_->enable_dscp};
  current_session_ = std::make_unique<SenderSession>(std::move(config));
  current_session_->SetStatsClient(this);

  AudioCaptureConfig audio_config;
  audio_config.bit_rate = 192 * 1000;
  // Use a larger playout delay (1s) to give the receiver more buffer room
  // and reduce black flashes from A/V desync.
  audio_config.target_playout_delay = milliseconds(1000);
  VideoCaptureConfig video_config = {
      .codec = connection_settings_->codec,
      .max_bit_rate =
          connection_settings_->max_bitrate - audio_config.bit_rate};
  video_config.target_playout_delay = milliseconds(1000);
  video_config.resolutions.emplace_back(Resolution{1920, 1080});

  OSP_LOG_INFO << "Negotiating session...";
  Error err = current_session_->Negotiate({audio_config}, {video_config});
  if (!err.ok()) {
    OSP_LOG_ERROR << "Negotiation failed: " << err;
    Shutdown();
  }
}

void X11CastAgent::OnNegotiated(
    const SenderSession* session,
    SenderSession::ConfiguredSenders senders,
    capture_recommendations::Recommendations capture_recommendations) {
  if (!senders.audio_sender || !senders.video_sender) {
    OSP_LOG_ERROR << "Missing audio or video sender";
    Shutdown();
    return;
  }

  current_negotiation_ =
      std::make_unique<SenderSession::ConfiguredSenders>(std::move(senders));
  StartX11Sender();
}

void X11CastAgent::OnError(const SenderSession* session,
                            const Error& error) {
  OSP_LOG_ERROR << "Session error: " << error;
  Shutdown();
}

void X11CastAgent::OnStatisticsUpdated(const SenderStats& updated_stats) {
  if ((num_stats_updates_++ % 10) == 0) {
    OSP_VLOG << "Stats: " << updated_stats;
  }
  last_stats_ = updated_stats;
}

void X11CastAgent::StartX11Sender() {
  OSP_LOG_INFO << "Starting X11 screen capture and streaming...";
  x11_sender_ = std::make_unique<X11Sender>(
      *environment_, connection_settings_.value(), current_session_.get(),
      std::move(*current_negotiation_),
      [this]() { shutdown_callback_(); },
      window_id_);
  current_negotiation_.reset();
}

void X11CastAgent::Shutdown() {
  if (shutting_down_) return;
  shutting_down_ = true;

  x11_sender_.reset();

  if (current_session_) {
    OSP_LOG_INFO << "Stopping session...";
    current_session_.reset();
    if (last_stats_) {
      OSP_LOG_INFO << "Final stats: " << *last_stats_;
    }
  }
  environment_.reset();

  if (platform_remote_connection_) {
    auto conn = *platform_remote_connection_;
    platform_remote_connection_.reset();
    connection_handler_.CloseRemoteConnection(conn);
  }

  if (remote_connection_) {
    auto conn = *remote_connection_;
    remote_connection_.reset();
    connection_handler_.CloseRemoteConnection(conn);
  }

  if (!app_session_id_.empty() &&
      message_port_.GetSocketId() != ToCastSocketId(nullptr)) {
    OSP_LOG_INFO << "Stopping Mirroring App...";
    static constexpr char kStopTemplate[] =
        R"({{"type":"STOP", "requestId":{}, "sessionId":"{}"}})";
    router_.Send(
        VirtualConnection{kPlatformSenderId, kPlatformReceiverId,
                          message_port_.GetSocketId()},
        MakeSimpleUTF8Message(
            kReceiverNamespace,
            StringFormat(kStopTemplate, next_request_id_++,
                         app_session_id_.c_str())));
    app_session_id_.clear();
  }

  if (message_port_.GetSocketId() != ToCastSocketId(nullptr)) {
    router_.CloseSocket(message_port_.GetSocketId());
    message_port_.SetSocket({});
  }

  wake_lock_.reset();

  if (shutdown_callback_) {
    auto cb = std::move(shutdown_callback_);
    cb();
  }
}

}  // namespace openscreen::cast
