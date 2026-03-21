// Copyright 2026

#include "cast/standalone_sender/controllable_file_cast_agent.h"

#include <sstream>
#include <string>
#include <utility>

#include "cast/common/channel/message_util.h"
#include "cast/common/public/cast_streaming_app_ids.h"
#include "cast/streaming/public/capture_recommendations.h"
#include "cast/streaming/public/constants.h"
#include "json/value.h"
#include "platform/api/tls_connection_factory.h"
#include "util/json/json_helpers.h"
#include "util/stringprintf.h"

namespace openscreen::cast {

namespace {
using DeviceMediaPolicy = SenderSocketFactory::DeviceMediaPolicy;
}  // namespace

ControllableFileCastAgent::ControllableFileCastAgent(
    TaskRunner& task_runner,
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

ControllableFileCastAgent::~ControllableFileCastAgent() {
  Shutdown();
}

void ControllableFileCastAgent::Connect(ConnectionSettings settings) {
  connection_settings_ = std::move(settings);
  const auto policy = connection_settings_->should_include_video
                          ? DeviceMediaPolicy::kIncludesVideo
                          : DeviceMediaPolicy::kAudioOnly;
  task_runner_.PostTask([this, policy] {
    if (!connection_settings_) return;
    socket_factory_.Connect(connection_settings_->receiver_endpoint, policy,
                            &router_);
  });
}

void ControllableFileCastAgent::Play() {
  desired_paused_ = false;
  if (sender_) {
    sender_->Play();
  }
}

void ControllableFileCastAgent::Pause() {
  desired_paused_ = true;
  if (sender_) {
    sender_->Pause();
    desired_position_ = sender_->GetCurrentPosition();
  }
}

void ControllableFileCastAgent::SeekTo(Clock::duration position) {
  desired_position_ = position;
  if (sender_) {
    sender_->SeekTo(position);
  }
}

void ControllableFileCastAgent::SeekBy(Clock::duration delta) {
  if (sender_) {
    sender_->SeekBy(delta);
    desired_position_ = sender_->GetCurrentPosition();
  } else {
    desired_position_ += delta;
  }
}

void ControllableFileCastAgent::SetViewport(const VideoViewport& viewport) {
  desired_viewport_ = viewport;
  if (sender_) {
    sender_->SetViewport(viewport);
  }
}

void ControllableFileCastAgent::ResetViewport() {
  desired_viewport_ = VideoViewport{};
  if (sender_) {
    sender_->ResetViewport();
  }
}

Clock::duration ControllableFileCastAgent::GetCurrentPosition() const {
  if (sender_) {
    return sender_->GetCurrentPosition();
  }
  return desired_position_;
}

Clock::duration ControllableFileCastAgent::GetDuration() const {
  if (sender_) {
    return sender_->GetDuration();
  }
  return Clock::duration::zero();
}

bool ControllableFileCastAgent::IsConnected() const {
  return sender_ != nullptr;
}

bool ControllableFileCastAgent::IsPlaying() const {
  if (sender_) {
    return sender_->is_playing();
  }
  return false;
}

std::string ControllableFileCastAgent::GetActiveModeString() const {
  if (sender_) {
    return sender_->GetActiveModeString();
  }
  if (connection_settings_) {
    return "connecting";
  }
  return "idle";
}

std::string ControllableFileCastAgent::GetDebugStateString() const {
  if (sender_) {
    return sender_->GetDebugStateString();
  }
  return {};
}

void ControllableFileCastAgent::SetAvSyncOffset(Clock::duration offset) {
  if (sender_) {
    sender_->SetAvSyncOffset(offset);
  }
}

void ControllableFileCastAgent::OnConnected(SenderSocketFactory* factory,
                                            const IPEndpoint& endpoint,
                                            std::unique_ptr<CastSocket> socket) {
  if (message_port_.GetSocketId() != ToCastSocketId(nullptr)) {
    return;
  }
  message_port_.SetSocket(socket->GetWeakPtr());
  router_.TakeSocket(this, std::move(socket));

  platform_remote_connection_.emplace(VirtualConnection{
      kPlatformSenderId, kPlatformReceiverId, message_port_.GetSocketId()});
  connection_handler_.OpenRemoteConnection(
      *platform_remote_connection_,
      [this](bool success) { OnReceiverMessagingOpened(success); });
}

void ControllableFileCastAgent::OnError(SenderSocketFactory* factory,
                                        const IPEndpoint& endpoint,
                                        const Error& error) {
  OSP_LOG_ERROR << "ControllableFileCastAgent socket factory error: " << error;
  std::ostringstream stream;
  stream << "socket_factory_error: " << error;
  Shutdown(stream.str());
}

void ControllableFileCastAgent::OnClose(CastSocket* cast_socket) {
  OSP_LOG_INFO << "ControllableFileCastAgent socket closed";
  Shutdown("socket_closed");
}

void ControllableFileCastAgent::OnError(CastSocket* socket,
                                        const Error& error) {
  OSP_LOG_ERROR << "ControllableFileCastAgent socket error: " << error;
  std::ostringstream stream;
  stream << "socket_error: " << error;
  Shutdown(stream.str());
}

bool ControllableFileCastAgent::IsConnectionAllowed(
    const VirtualConnection& virtual_conn) const {
  return true;
}

void ControllableFileCastAgent::OnMessage(VirtualConnectionRouter* router,
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
      Shutdown("launch_error");
    }
  }
}

void ControllableFileCastAgent::OnNegotiated(
    const SenderSession* session,
    SenderSession::ConfiguredSenders senders,
    capture_recommendations::Recommendations capture_recommendations) {
  current_negotiation_ =
      std::make_unique<SenderSession::ConfiguredSenders>(std::move(senders));
  StartSender();
}

void ControllableFileCastAgent::OnError(const SenderSession* session,
                                        const Error& error) {
  OSP_LOG_ERROR << "ControllableFileCastAgent session error: " << error;
  std::ostringstream stream;
  stream << "session_error: " << error;
  Shutdown(stream.str());
}

void ControllableFileCastAgent::OnStatisticsUpdated(
    const SenderStats& updated_stats) {}

const char* ControllableFileCastAgent::GetStreamingAppId() const {
  return connection_settings_ && !connection_settings_->should_include_video
             ? GetCastStreamingAudioOnlyAppId()
             : GetCastStreamingAudioVideoAppId();
}

void ControllableFileCastAgent::HandleReceiverStatus(const Json::Value& status) {
  const Json::Value& details =
      (status[kMessageKeyStatus].isObject() &&
       status[kMessageKeyStatus][kMessageKeyApplications].isArray())
          ? status[kMessageKeyStatus][kMessageKeyApplications][0]
          : Json::Value();

  std::string running_app_id;
  if (!json::TryParseString(details[kMessageKeyAppId], &running_app_id) ||
      running_app_id != GetStreamingAppId()) {
    if (has_launched_) {
      Shutdown("receiver_status_app_missing");
    }
    return;
  }

  has_launched_ = true;

  std::string session_id;
  if (!json::TryParseString(details[kMessageKeySessionId], &session_id) ||
      session_id.empty()) {
    Shutdown("receiver_status_missing_session");
    return;
  }
  if (app_session_id_.empty()) {
    app_session_id_ = session_id;
  }

  if (remote_connection_) {
    return;
  }

  std::string transport_id;
  if (!json::TryParseString(details[kMessageKeyTransportId], &transport_id) ||
      transport_id.empty()) {
    Shutdown("receiver_status_missing_transport");
    return;
  }

  remote_connection_.emplace(
      VirtualConnection{MakeUniqueSessionId("controlled_sender"), transport_id,
                        message_port_.GetSocketId()});
  connection_handler_.OpenRemoteConnection(
      *remote_connection_,
      [this](bool success) { OnRemoteMessagingOpened(success); });
}

void ControllableFileCastAgent::OnRemoteMessagingOpened(bool success) {
  if (success) {
    CreateAndStartSession();
  } else {
    Shutdown("remote_messaging_open_failed");
  }
}

void ControllableFileCastAgent::OnReceiverMessagingOpened(bool success) {
  if (!success) {
    Shutdown("receiver_messaging_open_failed");
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

void ControllableFileCastAgent::CreateAndStartSession() {
  if (!connection_settings_) {
    Shutdown("missing_connection_settings");
    return;
  }
  // Environment owns one UDP socket, so bind it to the same address family as
  // the chosen receiver endpoint instead of assuming IPv4.
  const IPEndpoint local_endpoint =
      connection_settings_->receiver_endpoint.address.IsV6()
          ? IPEndpoint::kAnyV6()
          : IPEndpoint::kAnyV4();
  environment_ = std::make_unique<Environment>(&Clock::now, task_runner_,
                                               local_endpoint);

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
  audio_config.target_playout_delay =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          connection_settings_->playout_delay);
  VideoCaptureConfig video_config = {
      .codec = connection_settings_->codec,
      .max_bit_rate =
          connection_settings_->max_bitrate - audio_config.bit_rate};
  video_config.target_playout_delay = audio_config.target_playout_delay;
  video_config.resolutions.emplace_back(Resolution{1920, 1080});

  const Error err = current_session_->Negotiate({audio_config}, {video_config});
  if (!err.ok()) {
    std::ostringstream stream;
    stream << "session_negotiate_error: " << err;
    Shutdown(stream.str());
  }
}

void ControllableFileCastAgent::StartSender() {
  if (!current_negotiation_) {
    return;
  }

  sender_ = std::make_unique<ControllableFileSender>(
      *environment_, connection_settings_.value(), current_session_.get(),
      std::move(*current_negotiation_),
      [this]() { Shutdown("sender_shutdown"); });
  current_negotiation_.reset();
  sender_->SetViewport(desired_viewport_);
  sender_->SeekTo(desired_position_);
  if (desired_paused_) {
    sender_->Pause();
  } else {
    sender_->Play();
  }
}

void ControllableFileCastAgent::Shutdown(const std::string& reason) {
  if (shutting_down_) {
    return;
  }
  shutting_down_ = true;

  sender_.reset();
  current_session_.reset();
  environment_.reset();

  if (platform_remote_connection_) {
    const VirtualConnection connection = *platform_remote_connection_;
    platform_remote_connection_.reset();
    connection_handler_.CloseRemoteConnection(connection);
  }
  if (remote_connection_) {
    const VirtualConnection connection = *remote_connection_;
    remote_connection_.reset();
    connection_handler_.CloseRemoteConnection(connection);
  }
  if (!app_session_id_.empty() &&
      message_port_.GetSocketId() != ToCastSocketId(nullptr)) {
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

  if (shutdown_callback_) {
    auto cb = std::move(shutdown_callback_);
    cb(reason);
  }
}

}  // namespace openscreen::cast
