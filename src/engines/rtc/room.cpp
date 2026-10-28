#include "engines/rtc/room.hpp"
#include "engines/rtc/signal_session.hpp"
#include "engines/rtc/client_configuration.hpp"
#include "engines/rtc/capabilities.hpp"
#include "engines/rtc/async_wait.hpp"
#include "engines/rtc/pending_requests.hpp"
#include "engines/rtc/data_messages.hpp"
#include "engines/rtc/subscription_permissions.hpp"
#include "support/net.hpp"
#include "backends/webrtc/headless_audio.hpp"
#include "media_nodes/render/video_playout.hpp"
#include "media_nodes/render/video_frames.hpp"
#include "backends/webrtc/codec_factories.hpp"
#include "backends/webrtc/audio_device.hpp"
#include "livekit.pb.h"
#include "api/create_peerconnection_factory.h"
#include "api/enable_media_with_defaults.h"
#include "api/audio/audio_device.h"
#include "api/make_ref_counted.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/rtp_transceiver_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "rtc_base/thread.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"
#include "media/sctp/dcsctp_transport.h"
#ifdef _WIN32
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/task_queue_win.h"
#endif
#include <chrono>
#include <atomic>
#include <future>
#include <map>
#include <set>
namespace lk {
namespace detail = livekit::sdk::detail;
using livekit::sdk::SDKError;
using livekit::sdk::ErrorCode;
using livekit::sdk::ErrorDomain;
using PC = webrtc::PeerConnectionInterface;
using Clock = std::chrono::steady_clock;
class SetDescription : public webrtc::SetSessionDescriptionObserver {
 public:
  std::promise<void> result;
  void OnSuccess() override { result.set_value(); }
  void OnFailure(webrtc::RTCError error) override { result.set_exception(std::make_exception_ptr(std::runtime_error(error.message()))); }
};
class CreateDescription : public webrtc::CreateSessionDescriptionObserver {
 public:
  std::promise<std::unique_ptr<webrtc::SessionDescriptionInterface>> result;
  void OnSuccess(webrtc::SessionDescriptionInterface* description) override { result.set_value(std::unique_ptr<webrtc::SessionDescriptionInterface>(description)); }
  void OnFailure(webrtc::RTCError error) override { result.set_exception(std::make_exception_ptr(std::runtime_error(error.message()))); }
};
class StatsObserver : public webrtc::RTCStatsCollectorCallback {
 public:
  StatsObserver(EventEmitter event,livekit::SignalTarget target,std::shared_ptr<std::atomic<bool>> pending)
      : event_(std::move(event)),target_(target),pending_(std::move(pending)) {}
  void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    try {
      Json rows=Json::array();
      // Keep native fields/types. Candidate addresses, certificate contents,
      // and credentials are not part of this media diagnostics interface.
      for (const auto& stat:*report) {
        std::string_view type=stat.type();
        if (type=="outbound-rtp" || type=="inbound-rtp" || type=="remote-inbound-rtp" ||
            type=="media-source" || type=="candidate-pair" || type=="transport")
          rows.push_back(Json::parse(stat.ToJson()));
      }
      event_("rtc-stats",{{"target",int(target_)},{"timestampUs",report->timestamp().us()},{"stats",std::move(rows)}});
    } catch (...) {
      // Diagnostics must not unwind through libwebrtc's callback or stop media.
    }
    pending_->store(false);
  }
 private:
  EventEmitter event_;
  livekit::SignalTarget target_;
  std::shared_ptr<std::atomic<bool>> pending_;
};
// The local WebRTC manifest builds DcSctpTransport but does not enable the
// default factory's WEBRTC_HAVE_DCSCTP branch. Supply the native factory
// explicitly through the public dependency injection interface.
class SctpFactory : public webrtc::SctpTransportFactoryInterface {
 public:
  explicit SctpFactory(webrtc::Thread* network) : network_(network) {}
  std::unique_ptr<webrtc::SctpTransportInternal> CreateSctpTransport(
      const webrtc::Environment& env, webrtc::DtlsTransportInternal* dtls) override {
    return std::make_unique<webrtc::DcSctpTransport>(env, network_, dtls);
  }
 private:
  webrtc::Thread* network_;
};
class Runtime {
 public:
#ifdef _WIN32
  webrtc::WinsockInitializer winsock;
#endif
  std::unique_ptr<webrtc::Thread> network, worker, signaling;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
  std::shared_ptr<livekit::sdk::detail::RtcGraph> graph = std::make_shared<livekit::sdk::detail::RtcGraph>();
  webrtc::scoped_refptr<livekit::sdk::detail::GraphAudioDevice> audio_device;
  Runtime(bool hardware, bool microphone = false) {
#ifdef _WIN32
    if (winsock.error()) throw std::runtime_error("Winsock initialization failed");
#endif
    if (!webrtc::InitializeSSL()) throw std::runtime_error("WebRTC SSL initialization failed");
    ssl_ = true;
    try {
      network = webrtc::Thread::CreateWithSocketServer(); worker = webrtc::Thread::Create(); signaling = webrtc::Thread::Create();
      if (!network->Start() || !worker->Start() || !signaling->Start()) throw std::runtime_error("Cannot start WebRTC threads");
      webrtc::PeerConnectionFactoryDependencies deps;
      deps.network_thread = network.get(); deps.worker_thread = worker.get(); deps.signaling_thread = signaling.get();
      deps.sctp_factory = std::make_unique<SctpFactory>(network.get());
#ifdef _WIN32
      deps.task_queue_factory = webrtc::CreateTaskQueueWinFactory();
#else
      deps.task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();
#endif
      deps.adm = worker->BlockingCall([&]() -> webrtc::scoped_refptr<webrtc::AudioDeviceModule> {
        if (hardware) return webrtc::AudioDeviceModule::Create(webrtc::AudioDeviceModule::kPlatformDefaultAudio, deps.task_queue_factory.get());
        return webrtc::make_ref_counted<HeadlessAudioDevice>();
      });
      if (!deps.adm) throw std::runtime_error("Cannot create WebRTC audio device");
      audio_device = webrtc::make_ref_counted<livekit::sdk::detail::GraphAudioDevice>(deps.adm, graph, microphone);
      deps.adm = audio_device;
      livekit::sdk::detail::install_graph_codecs(deps, graph);
      webrtc::EnableMediaWithDefaults(deps);
      factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
      if (!factory) throw std::runtime_error("Cannot create WebRTC factory");
      // SDK clients must also connect reliably to local development servers.
      // WebRTC's browser-oriented default excludes loopback interfaces.
      webrtc::PeerConnectionFactoryInterface::Options factory_options;
      factory_options.network_ignore_mask = 0;
      factory->SetOptions(factory_options);
    } catch (...) { cleanup(); throw; }
  }
  ~Runtime() { cleanup(); }
 private:
  bool ssl_ = false;
  void cleanup() {
    graph->close();
    if (factory) signaling->BlockingCall([&] { factory = nullptr; });
    if (audio_device && worker) worker->BlockingCall([&] { audio_device = nullptr; });
    if (signaling) signaling->Stop(); if (worker) worker->Stop(); if (network) network->Stop();
    if (ssl_) { webrtc::CleanupSSL(); ssl_ = false; }
  }
};
class Peer : public webrtc::PeerConnectionObserver, public webrtc::DataChannelObserver {
 public:
  webrtc::scoped_refptr<PC> pc;
  std::atomic<bool> connected{false}, failed{false}, transport_failed{false};
  const livekit::SignalTarget target;
  Peer(Runtime& runtime, SignalSession& socket, livekit::SignalTarget which, const PC::RTCConfiguration& config, std::filesystem::path record, EventEmitter events, VideoPlayout* playout = nullptr, VideoFrames* frames = nullptr, std::shared_ptr<detail::AudioFrameOutput> audio = {})
      : target(which), event(std::move(events)), socket_(socket), record_(std::move(record)), playout_(playout), frames_(frames), graph_(runtime.graph), audio_output_(std::move(audio)) {
    auto result = runtime.factory->CreatePeerConnectionOrError(config, webrtc::PeerConnectionDependencies(this));
    if (!result.ok()) throw std::runtime_error(result.error().message());
    pc = result.MoveValue();
  }
  ~Peer() {
    if (pc) pc->Close();
    for (auto& channel : channels_) channel->UnregisterObserver();
    for (auto& sink : sinks_) { sink->detach(); event("track-summary", sink->stats()); }
    sinks_.clear(); channels_.clear(); pc = nullptr;
  }
  void OnSignalingChange(PC::SignalingState) override {}
  void OnIceGatheringChange(PC::IceGatheringState) override {}
  void OnRenegotiationNeeded() override {}
  void OnConnectionChange(PC::PeerConnectionState state) override {
    connected = state == PC::PeerConnectionState::kConnected;
    transport_failed = state == PC::PeerConnectionState::kFailed;
    event("transport", {{"target", static_cast<int>(target)}, {"state", static_cast<int>(state)}});
  }
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
    try {
      std::string sdp; candidate->ToString(&sdp);
      livekit::SignalRequest request;
      request.mutable_trickle()->set_target(target);
      request.mutable_trickle()->set_candidateinit(Json{{"candidate", sdp}, {"sdpMid", candidate->sdp_mid()}, {"sdpMLineIndex", candidate->sdp_mline_index()}}.dump());
      socket_.enqueue_ice(request);
    } catch (...) { failed = true; }
  }
  void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
    try {
      auto track = transceiver->receiver()->track();
      Json streams = Json::array();
      auto sid = track->id();
      for (const auto& stream : transceiver->receiver()->streams()) {
        const auto id = stream->id(); streams.push_back(id);
        // LiveKit encodes participant SID|track SID in the stream ID. A reused
        // receiver retains its original native track ID across renegotiation.
        const auto separator = id.find('|');
        if (separator != std::string::npos && id.starts_with("PA_") && id.substr(separator + 1).starts_with("TR_"))
          sid = id.substr(separator + 1);
      }
      std::vector<uint32_t> ssrcs;
      if (track->kind() == "video") {
        for (const auto& encoding : transceiver->receiver()->GetParameters().encodings)
          if (encoding.ssrc) ssrcs.push_back(*encoding.ssrc);
      }
      auto sink = std::make_unique<TrackSink>(track, record_, playout_, frames_, sid, graph_, std::move(ssrcs), audio_output_);
      sink->attach();
      std::lock_guard lock(mutex_); sinks_.push_back(std::move(sink));
      event("track-subscribed", {{"track", sid}, {"kind", track->kind()}, {"mid", transceiver->mid().value_or("")}, {"streams", streams}});
    } catch (const std::exception& e) { event("media-error", {{"message", e.what()}}); failed = true; }
  }
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {
    channel->RegisterObserver(this); channels_.push_back(channel);
  }
  void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override {
    std::lock_guard lock(mutex_);
    for (auto it = sinks_.begin(); it != sinks_.end();) {
      if ((*it)->owns_track(receiver->track().get())) {
        (*it)->detach(); event("track-summary", (*it)->stats());
        event("track-unsubscribed", {{"track", (*it)->track_id()}});
        it = sinks_.erase(it);
      } else ++it;
    }
  }
  void OnStateChange() override {}
  void OnMessage(const webrtc::DataBuffer& buffer) override {
    if (buffer.data.size()>16384) return;
    livekit::DataPacket packet;
    if (!packet.ParseFromArray(buffer.data.data(), static_cast<int>(buffer.data.size()))) return;
    if(packet.has_rpc_request()||packet.has_rpc_ack()||packet.has_rpc_response())
      event("rpc",{{"payload",base64url(packet.SerializeAsString())}});
    try {
      if (packet.has_sip_dtmf()) {
        livekit::sdk::DtmfDigit digit{packet.sip_dtmf().code(),packet.sip_dtmf().digit()};
        livekit::sdk::detail::validate_dtmf(digit);
        event("dtmf", {{"code",digit.code},{"digit",digit.digit},{"identity",packet.participant_identity()},{"reliable",packet.kind()==0}});
      } else if (packet.has_transcription()) {
        const auto value=livekit::sdk::detail::transcription_json(packet.transcription());
        livekit::sdk::detail::parse_transcription(value);
        event("transcription", {{"value",value},{"identity",packet.participant_identity()},{"reliable",packet.kind()==0}});
      }
    } catch (const std::invalid_argument&) { return; }
    if (packet.has_user())
      event("data", {{"identity", packet.participant_identity().empty() ? packet.user().participant_identity() : packet.participant_identity()}, {"reliable", packet.kind() == 0}, {"topic", packet.user().topic()}, {"payloadBase64", base64url(packet.user().payload())}});
    if (packet.has_speaker()) {
      Json speakers = Json::array();
      for (const auto& p : packet.speaker().speakers()) speakers.push_back({{"sid", p.sid()}, {"level", p.level()}, {"active", p.active()}});
      event("speakers", {{"speakers", speakers}, {"replace", true}});
    }
  }
  void check_recordings() {
    std::lock_guard lock(mutex_);
    for (auto& sink : sinks_) { auto error = sink->error(); if (!error.empty()) throw std::runtime_error(error); }
  }
  void request_stats() {
    if (!connected || stats_pending_->exchange(true)) return;
    auto observer=webrtc::make_ref_counted<StatsObserver>(event,target,stats_pending_);
    pc->GetStats(observer.get());
  }
  livekit::SessionDescription last_local, last_remote;
  // Called on the signaling thread, which owns channels_.
  void append_data_channels(livekit::SyncState& state) const {
    for (const auto& channel : channels_) if (channel->id() >= 0) {
      auto* info = state.add_data_channels();
      info->set_label(channel->label()); info->set_id(channel->id()); info->set_target(target);
    }
  }
  void remote(const livekit::SessionDescription& description) {
    webrtc::SdpParseError error;
    auto type = webrtc::SdpTypeFromString(description.type());
    if (!type) throw std::runtime_error("Invalid SDP type");
    auto sdp = webrtc::CreateSessionDescription(*type, description.sdp(), &error);
    if (!sdp) throw std::runtime_error("Invalid remote SDP: " + error.description);
    auto observer = webrtc::make_ref_counted<SetDescription>(); auto future = observer->result.get_future();
    pc->SetRemoteDescription(observer.get(), sdp.release());
    detail::await_native(future, socket_.stop_token(), socket_.operation_deadline());
    last_remote = description;
    remote_set_ = true;
    for (const auto& candidate : pending_ice_) add_ice(candidate);
    pending_ice_.clear();
  }
  livekit::SessionDescription local(bool offer, bool ice_restart = false) {
    // CreateDescription and SetLocalDescription share one operation budget.
    const auto deadline = socket_.operation_deadline();
    auto creator = webrtc::make_ref_counted<CreateDescription>(); auto future = creator->result.get_future();
    PC::RTCOfferAnswerOptions options;
    options.ice_restart = ice_restart;
    if (offer) pc->CreateOffer(creator.get(), options); else pc->CreateAnswer(creator.get(), options);
    auto sdp = detail::await_native(future, socket_.stop_token(), deadline);
    livekit::SessionDescription result; result.set_type(offer ? "offer" : "answer"); sdp->ToString(result.mutable_sdp());
    auto setter = webrtc::make_ref_counted<SetDescription>(); auto set_result = setter->result.get_future();
    pc->SetLocalDescription(setter.get(), sdp.release());
    detail::await_native(set_result, socket_.stop_token(), deadline); last_local = result; return result;
  }
  void ice(const livekit::TrickleRequest& request) {
    // LiveKit may mark the last actual candidate as final. In a TCP-only
    // deployment this can be the sole usable candidate, not an empty marker.
    if (request.candidateinit().empty()) return;
    if (!remote_set_) pending_ice_.push_back(request.candidateinit()); else add_ice(request.candidateinit());
  }
 private:
  void add_ice(const std::string& text) {
    auto json = Json::parse(text);
    auto candidate_text = json.value("candidate", ""); if (candidate_text.empty()) return;
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> candidate(webrtc::CreateIceCandidate(json.value("sdpMid", ""), json.value("sdpMLineIndex", 0), candidate_text, &error));
    if (!candidate) throw std::runtime_error("Invalid ICE candidate: " + error.description);
    if (!pc->AddIceCandidate(candidate.get())) throw std::runtime_error("WebRTC rejected ICE candidate");
  }
  EventEmitter event;
  SignalSession& socket_;
  std::filesystem::path record_;
  VideoPlayout* playout_ = nullptr;
  VideoFrames* frames_ = nullptr;
  std::shared_ptr<livekit::sdk::detail::RtcGraph> graph_;
  std::shared_ptr<detail::AudioFrameOutput> audio_output_;
  std::mutex mutex_;
  std::vector<std::unique_ptr<TrackSink>> sinks_;
  std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> channels_;
  std::vector<std::string> pending_ice_;
  bool remote_set_ = false;
  std::shared_ptr<std::atomic<bool>> stats_pending_=std::make_shared<std::atomic<bool>>(false);
};
void webrtc_smoke(EventEmitter event) {
  Runtime runtime(false); SignalSession socket(RoomOptions{}, {}, event);
  auto peer = std::make_unique<Peer>(runtime, socket, livekit::PUBLISHER, PC::RTCConfiguration{}, std::filesystem::path{}, event);
  try {
    auto channel = peer->pc->CreateDataChannelOrError("_reliable", nullptr);
    if (!channel.ok()) throw std::runtime_error(channel.error().message());
    auto description = peer->local(true);
    if (description.sdp().find("m=application") == std::string::npos) throw std::runtime_error("Data channel SDP missing");
  } catch (...) {
    runtime.signaling->BlockingCall([&] { peer.reset(); });
    throw;
  }
  runtime.signaling->BlockingCall([&] { peer.reset(); });
  event("webrtc-smoke", {{"ok", true}});
}
Json track_json(const livekit::TrackInfo& t) {
  return {{"sid", t.sid()}, {"name", t.name()}, {"type", static_cast<int>(t.type())}, {"source", static_cast<int>(t.source())}, {"muted", t.muted()}, {"width", t.width()}, {"height", t.height()}, {"mimeType", t.mime_type()}, {"mid", t.mid()}};
}
Json participant_json(const livekit::ParticipantInfo& p) {
  Json tracks = Json::array(), attributes = Json::object();
  for (const auto& t : p.tracks()) tracks.push_back(track_json(t));
  for (const auto& [key, value] : p.attributes()) attributes[key] = value;
  return {{"sid", p.sid()}, {"identity", p.identity()}, {"name", p.name()}, {"metadata", p.metadata()}, {"state", p.state()}, {"kind", p.kind()}, {"disconnectReason", p.disconnect_reason()}, {"attributes", attributes}, {"tracks", tracks}};
}
void join_room(const RoomOptions& options, std::stop_token stop, EventEmitter event, std::shared_ptr<livekit::sdk::detail::RoomState> session) {
  if (stop.stop_requested()) return;
  detail::RecoveryState standalone;
  auto& recovery = session ? session->recovery : standalone;
  const bool restore_session = recovery.rejoining;
  if (!session) {
    recovery.subscription_permissions=options.subscription_permissions;
    recovery.token = options.token; recovery.expected_room = options.room; recovery.budget = detail::RecoveryBudget(options.reconnect);
    for (const auto& media : options.media) recovery.publications.emplace(recovery.allocate_id(media.video), detail::DesiredPublication{media});
    if (options.microphone) recovery.publications.emplace("microphone", detail::DesiredPublication{{}, {}, true});
  }
  SignalSession socket(options, stop, event, &recovery);
  socket.connect();
  livekit::JoinResponse join;
  while (!stop.stop_requested()) {
    try {
    recovery.budget.check();
    event.dispatch(); event.rethrow();
    auto message = socket.receive(100);
    if (message) {
      livekit::SignalResponse response;
      if (!response.ParseFromString(*message)) throw std::runtime_error("Invalid LiveKit signal response");
      if (response.has_leave()) throw SDKError(ErrorDomain::signaling, ErrorCode::session_closed,
          "Server rejected join", !detail::terminal_leave(response.leave().reason()) && response.leave().can_reconnect(), response.leave().reason());
      if (response.has_join()) {
        if (!response.join().alternative_url().empty()) { socket.redirect(response.join().alternative_url()); continue; }
        join = response.join(); break;
      }
    }
    if (Clock::now() >= socket.connect_deadline()) throw SDKError(ErrorDomain::signaling, ErrorCode::timeout, "Timed out waiting for LiveKit JoinResponse", true);
    } catch(const SDKError& error) { if(!socket.failover(error))throw; }
  }
  if (stop.stop_requested()) return;
  if (join.participant().sid().empty()) throw std::runtime_error("Join response has no participant");
  socket.bind_participant(join.participant().sid());
  if (!recovery.expected_room.empty() && join.room().name() != recovery.expected_room) throw std::runtime_error("Token room does not match the requested room");
  recovery.expected_room = join.room().name();
  auto client_configuration = join.client_configuration();
  socket.configure_client(client_configuration);
  const bool relay_only = detail::relay_required(options.relay_only, options.disable_turn, client_configuration);
  Json roster = Json::array(); for (const auto& p : join.other_participants()) roster.push_back(participant_json(p));
  event("joined", {{"room", join.room().name()}, {"identity", join.participant().identity()}, {"sid", join.participant().sid()},
    {"roomInfo", {{"sid", join.room().sid()}, {"name", join.room().name()}, {"metadata", join.room().metadata()}, {"recording", join.room().active_recording()}}},
    {"localParticipant", participant_json(join.participant())}, {"participants", roster}});
  event("session-capabilities",detail::session_capabilities_json(join,options.signal_protocol,relay_only));
  for (const auto& p : join.other_participants())
    event("participant", {{"identity", p.identity()}, {"sid", p.sid()}, {"state", p.state()}, {"participant", participant_json(p)}, {"initial", true}});
  const bool restore_microphone=std::any_of(recovery.publications.begin(),recovery.publications.end(),[](const auto& p){return p.second.microphone;});
  Runtime runtime(options.audio_playout || restore_microphone, restore_microphone);
  std::unique_ptr<VideoFrames> video_frames;
  if (options.video_frame_handler || options.video_frame_stream)
    video_frames = std::make_unique<VideoFrames>(options.video_frame_handler, session ? session->video_output : nullptr);
  std::unique_ptr<VideoPlayout> playout;
  if (options.video_playout) playout = std::make_unique<VideoPlayout>(event);
  PC::RTCConfiguration config; config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  for (const auto& ice : join.ice_servers()) {
    PC::IceServer server; for (const auto& url : ice.urls()) if (!options.disable_turn || (!url.starts_with("turn:") && !url.starts_with("turns:"))) server.urls.push_back(url); server.username = ice.username(); server.password = ice.credential(); if (!server.urls.empty()) config.servers.push_back(std::move(server));
  }
  if (relay_only) config.type = PC::kRelay;
  std::unique_ptr<Peer> publisher, subscriber;
  std::vector<std::shared_ptr<MediaPump>> pumps;
  std::vector<webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>> tracks;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> reliable, lossy_channel;
  using Command = livekit::sdk::detail::Command;
  std::map<std::string, std::shared_ptr<Command>> publishing;
  detail::PendingRequests pending_controls;
  std::map<std::string, Json> remote_settings;
  std::deque<std::shared_ptr<Command>> deferred;
  std::map<std::string, MediaPump*> track_pumps;
  std::set<MediaPump*> started_pumps;
  std::map<std::string, webrtc::scoped_refptr<webrtc::RtpSenderInterface>> senders;
  auto cleanup = [&] {
    pending_controls.interrupt("Connection closed before request confirmation; outcome is unknown");
    runtime.graph->close();
    for (auto& [id, command] : publishing) command->interrupt(ErrorCode::session_closed,
        "Room disconnected before publication confirmation; outcome is unknown");
    for (auto& command : deferred) command->interrupt(ErrorCode::session_closed, "Room disconnected before command completed");
    for (auto& pump : pumps) pump->stop();
    try { livekit::SignalRequest leave; leave.mutable_leave(); socket.send(leave); } catch (...) {}
    socket.close();
    runtime.signaling->BlockingCall([&] { reliable = nullptr; lossy_channel = nullptr; senders.clear(); publisher.reset(); subscriber.reset(); tracks.clear(); });
    for (const auto& pump : pumps) event("publish-summary", {{"kind", pump->spec.video ? "video" : "audio"}, {"frames", pump->frames.load()}});
    pumps.clear();
  };
  try {
    publisher = std::make_unique<Peer>(runtime, socket, livekit::PUBLISHER, config, std::filesystem::path{}, event);
    auto recording = options.record;
    if (!recording.empty() && recovery.rejoining) recording /= "rejoin-" + std::to_string(session ? session->generation : 0);
    subscriber = std::make_unique<Peer>(runtime, socket, livekit::SUBSCRIBER, config, recording, event, playout.get(), video_frames.get(), session ? session->audio_output : nullptr);
    std::set<std::string> awaiting_tracks;
    std::map<std::string, webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>> published_tracks;
    std::map<std::string, livekit::TrackPublishedResponse> published_info;
    std::map<std::string, bool> subscription_overrides;
    if (!recovery.room_changed) for (const auto& sid : options.subscribe_tracks) subscription_overrides[sid] = true;
    auto send = [&](const livekit::SignalRequest& request) { socket.send(request); };
    auto send_permissions = [&] {
      if(!recovery.subscription_permissions)return;
      livekit::SignalRequest request;
      *request.mutable_subscription_permission()=detail::permissions_wire(*recovery.subscription_permissions,recovery.local_ids);
      send(request);
    };
    // Enforce the policy before initial/recovered publication. Specific source
    // grants start denied until TrackPublishedResponse supplies the new SID.
    send_permissions();
    auto send_settings = [&](const std::string& sid, const Json& values) {
      livekit::SignalRequest request;
      auto* settings = request.mutable_track_setting(); settings->add_track_sids(sid);
      settings->set_disabled(values.value("disabled", false)); settings->set_quality(values.value("quality", 2));
      settings->set_width(values.value("width", 0)); settings->set_height(values.value("height", 0));
      send(request);
    };
    auto remember_remote = [&](const std::string& sid, std::optional<bool> subscribed, const Json& settings) {
      if (!session) return;
      std::lock_guard lock(session->mutex);
      for (const auto& [identity, p] : session->remote) for (const auto& t : p.tracks) if (t.sid == sid) {
        auto found = std::find_if(recovery.remote.begin(), recovery.remote.end(), [&](const auto& entry) {
          return entry.second.sid == sid && entry.second.identity == identity;
        });
        auto& intent = found == recovery.remote.end() ? recovery.remote[sid] : found->second;
        intent.identity = identity; intent.sid = sid; intent.name = t.name; intent.kind = t.kind; intent.source = t.source;
        if (subscribed) intent.subscribed = subscribed;
        intent.settings.update(settings);
        return;
      }
    };
    std::map<std::string, std::string> restored_intents;
    auto restore_remote = [&](const livekit::ParticipantInfo& participant) {
      if (!restore_session) return;
      for (auto& [key, intent] : recovery.remote) {
        if (intent.identity != participant.identity() || participant.state() == 3) continue;
        const livekit::TrackInfo* match = nullptr;
        int matches = 0;
        for (const auto& t : participant.tracks()) {
          if (t.sid() == intent.sid) { match = &t; matches = 1; break; }
          if (t.name() == intent.name && static_cast<int>(t.type()) == static_cast<int>(intent.kind) &&
              static_cast<int>(t.source()) == static_cast<int>(intent.source)) { match = &t; ++matches; }
        }
        if (!matches) continue;
        if (matches != 1) throw SDKError(ErrorDomain::lifecycle, ErrorCode::recovery_failed, "Ambiguous remote track when restoring settings");
        if (restored_intents[key] == match->sid()) continue;
        intent.sid = match->sid();
        if (intent.subscribed.has_value()) {
          livekit::SignalRequest request;
          request.mutable_subscription()->add_track_sids(intent.sid);
          request.mutable_subscription()->set_subscribe(*intent.subscribed); send(request);
          subscription_overrides[intent.sid] = *intent.subscribed;
        }
        if (!intent.settings.empty()) { send_settings(intent.sid, intent.settings); remote_settings[intent.sid] = intent.settings; }
        restored_intents[key] = intent.sid;
      }
    };
    auto add_track = [&](webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, const MediaSpec* spec, const livekit::sdk::TrackPublishOptions& publication = {}) {
      webrtc::RtpTransceiverInit init; init.direction = webrtc::RtpTransceiverDirection::kSendOnly; init.stream_ids = {publication.stream.empty() ? "livekit-sdk" : publication.stream};
      webrtc::RtpEncodingParameters encoding;
      encoding.max_bitrate_bps = track->kind() == "video" ? 2000000 : 64000;
      if (spec && spec->video) encoding.max_framerate = spec->fps;
      init.send_encodings.push_back(encoding);
      auto result = publisher->pc->AddTransceiver(track, init);
      if (!result.ok()) throw std::runtime_error(result.error().message());
      if (track->kind() == "audio" && publication.disable_red) {
        auto capabilities = runtime.factory->GetRtpSenderCapabilities(webrtc::MediaType::AUDIO);
        std::vector<webrtc::RtpCodecCapability> selected;
        for (const auto& codec : capabilities.codecs) {
          auto name = codec.name;
          std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
          if (name != "red") selected.push_back(codec);
        }
        auto error = result.value()->SetCodecPreferences(selected);
        if (!error.ok()) throw std::runtime_error(error.message());
      }
      if (track->kind() == "video") {
        auto capabilities = runtime.factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
        std::vector<webrtc::RtpCodecCapability> selected;
        bool found_codec = false;
        for (auto codec : capabilities.codecs) { auto name = codec.name; std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); found_codec |= name == options.codec; if (name == options.codec || name == "rtx" || name == "red" || name == "ulpfec") selected.push_back(codec); }
        if (!found_codec) throw std::runtime_error("Video codec not available: " + options.codec);
        auto error = result.value()->SetCodecPreferences(selected); if (!error.ok()) throw std::runtime_error(error.message());
      }
      senders[track->id()] = result.value()->sender();
      tracks.push_back(track); awaiting_tracks.insert(track->id());
      livekit::SignalRequest request; auto* add = request.mutable_add_track();
      add->set_cid(track->id()); add->set_name(publication.name.empty() ? track->id() : publication.name); add->set_stream(init.stream_ids.front());
      add->set_type(track->kind() == "video" ? livekit::VIDEO : livekit::AUDIO);
      if (track->kind() == "audio") add->set_disable_red(publication.disable_red);
      if (spec && spec->audio_source) add->set_stereo(spec->audio_source->options().channels == 2);
      const auto default_source = spec && spec->capture == livekit::sdk::CaptureKind::screen ? livekit::SCREEN_SHARE :
          track->kind() == "video" ? livekit::CAMERA : livekit::MICROPHONE;
      add->set_source(publication.source == livekit::sdk::TrackSource::unknown ? default_source : static_cast<livekit::TrackSource>(publication.source));
      if (spec && spec->video) { add->set_width(spec->width); add->set_height(spec->height); auto layer = add->add_layers(); layer->set_quality(2); layer->set_width(spec->width); layer->set_height(spec->height); layer->set_bitrate(2000000); }
      send(request);
    };
    for (const auto& [id, desired] : recovery.publications) {
      if (desired.microphone) {
        auto source = runtime.factory->CreateAudioSource(webrtc::AudioOptions{});
        auto track = runtime.factory->CreateAudioTrack(id, source.get());
        track->set_enabled(!desired.muted); add_track(track, nullptr, desired.options);
      } else {
        const auto& spec = desired.media;
        auto pump = std::make_shared<MediaPump>(spec, runtime.graph, playout.get(), video_frames.get(), id);
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track;
        if (spec.video) track = runtime.factory->CreateVideoTrack(pump->video, id);
        else track = runtime.factory->CreateAudioTrack(id, pump->audio.get());
        track->set_enabled(!desired.muted); add_track(track, &spec, desired.options);
        track_pumps[id] = pump.get(); pumps.push_back(std::move(pump));
      }
    }
    bool need_publisher = !tracks.empty() || !options.publish_data.empty() || !join.subscriber_primary();
    if (session || need_publisher) {
      for (bool lossy : {false, true}) {
        webrtc::DataChannelInit init; init.ordered = true; if (lossy) init.maxRetransmits = 0;
        auto channel = publisher->pc->CreateDataChannelOrError(lossy ? "_lossy" : "_reliable", &init);
        if (!channel.ok()) throw std::runtime_error(channel.error().message());
        if (!lossy) reliable = channel.value(); else lossy_channel = channel.value();
      }
    }
    if (!options.subscribe_tracks.empty() && !recovery.room_changed) {
      livekit::SignalRequest request; auto* subscription = request.mutable_subscription(); subscription->set_subscribe(true);
      for (const auto& sid : options.subscribe_tracks) subscription->add_track_sids(sid); send(request);
      if (!restore_session) for (const auto& participant : join.other_participants()) for (const auto& track : participant.tracks())
        if (std::find(options.subscribe_tracks.begin(), options.subscribe_tracks.end(), track.sid()) != options.subscribe_tracks.end()) {
          detail::RemoteIntent intent;
          intent.identity = participant.identity(); intent.sid = track.sid(); intent.name = track.name();
          intent.kind = static_cast<livekit::sdk::TrackKind>(track.type()); intent.source = static_cast<livekit::sdk::TrackSource>(track.source());
          intent.subscribed = true; recovery.remote[track.sid()] = std::move(intent);
        }
    }
    for (const auto& participant : join.other_participants()) restore_remote(participant);
    if (recovery.rejoining && recovery.local_snapshot) {
      const auto& desired = *recovery.local_snapshot;
      livekit::SignalRequest request;
      auto* update = request.mutable_update_metadata();
      bool changed = false;
      if (desired.name != join.participant().name() && !desired.name.empty()) { update->set_name(desired.name); changed = true; }
      if (desired.metadata != join.participant().metadata() && !desired.metadata.empty()) { update->set_metadata(desired.metadata); changed = true; }
      for (const auto& [key, value] : desired.attributes) {
        auto found = join.participant().attributes().find(key);
        if (found == join.participant().attributes().end() || found->second != value) { (*update->mutable_attributes())[key] = value; changed = true; }
      }
      for (const auto& [key, value] : join.participant().attributes())
        if (!desired.attributes.contains(key)) { (*update->mutable_attributes())[key] = ""; changed = true; }
      if (changed) send(request);
    }
    bool negotiated = false, dirty = true, offer_pending = false, pumping = false, connected = false,
         data_sent = options.publish_data.empty() || recovery.initial_data_sent;
    auto remove_track = [&](const std::string& sid) {
      auto found = published_tracks.find(sid); if (found == published_tracks.end()) return;
      auto cid = found->second->id(); auto sender = senders.find(cid);
      if (sender != senders.end()) { auto error = publisher->pc->RemoveTrackOrError(sender->second); if (!error.ok()) throw std::runtime_error(error.message()); senders.erase(sender); }
      auto pump = track_pumps.find(cid);
      if (pump != track_pumps.end()) { pump->second->stop(); started_pumps.erase(pump->second); std::erase_if(pumps, [&](const auto& p) { return p.get() == pump->second; }); track_pumps.erase(pump); }
      std::erase_if(tracks, [&](const auto& t) { return t->id() == cid; });
      const bool is_microphone=(recovery.publications.contains(cid)&&recovery.publications.at(cid).microphone) || (publishing.contains(cid)&&publishing.at(cid)->operation=="publish-microphone");
      if(is_microphone)runtime.worker->BlockingCall([&]{runtime.audio_device->disable_capture();});
      recovery.publications.erase(cid); recovery.local_ids.erase(sid);
      published_tracks.erase(found); published_info.erase(sid); dirty = true; event("track-unpublished", {{"sid", sid}});
    };
    auto joined_at = Clock::now(), last_ping = joined_at, last_pong = joined_at, connected_at = joined_at;
    auto next_stats=joined_at;
    auto ping_interval = std::chrono::seconds(join.ping_interval() > 0 ? join.ping_interval() : 5);
    auto ping_timeout = std::chrono::seconds(join.ping_timeout() > 0 ? join.ping_timeout() : 15);
    bool recovering = false;
    auto recovery_deadline = Clock::time_point::max();
    auto build_sync = [&](livekit::SyncState& sync) {
      *sync.mutable_answer() = subscriber->last_local;
      *sync.mutable_offer() = subscriber->last_remote;
      const bool automatic = options.subscribe && options.subscribe_tracks.empty();
      auto* subscription = sync.mutable_subscription(); subscription->set_subscribe(!automatic);
      for (const auto& [sid, enabled] : subscription_overrides)
        if (enabled != automatic) subscription->add_track_sids(sid);
      for (const auto& [sid, info] : published_info) *sync.add_publish_tracks() = info;
      for (const auto& [sid, settings] : remote_settings)
        if (settings.value("disabled", false)) sync.add_track_sids_disabled(sid);
      for (const auto& channel : {reliable, lossy_channel}) if (channel && channel->id() >= 0) {
        auto* info = sync.add_data_channels(); info->set_label(channel->label());
        info->set_id(channel->id()); info->set_target(livekit::PUBLISHER);
      }
      runtime.signaling->BlockingCall([&] { subscriber->append_data_channels(sync); });
    };
    auto configure_resume = [&](const livekit::ReconnectResponse& response) {
      // An omitted configuration doesn't silently undo an earlier relay policy.
      if (response.has_client_configuration()) client_configuration = response.client_configuration();
      const bool relay = detail::relay_required(options.relay_only, options.disable_turn, client_configuration);
      socket.configure_client(client_configuration);
      for (const auto& peer : {publisher.get(), subscriber.get()}) {
        auto updated = peer->pc->GetConfiguration();
        updated.type = relay ? PC::kRelay : PC::kAll;
        updated.servers.clear();
        for (const auto& ice : response.ice_servers()) {
          PC::IceServer server;
          for (const auto& url : ice.urls()) if (!options.disable_turn || (!url.starts_with("turn:") && !url.starts_with("turns:"))) server.urls.push_back(url);
          server.username = ice.username(); server.password = ice.credential();
          if (!server.urls.empty()) updated.servers.push_back(std::move(server));
        }
        const auto error = peer->pc->SetConfiguration(updated);
        if (!error.ok()) throw detail::RejoinRequired("Unable to apply refreshed ICE configuration");
      }
      Json capabilities{{"relayOnly",relay}};
      if(response.has_server_info())capabilities["server"]=detail::server_capabilities_json(response.server_info());
      event("session-capabilities",capabilities);
    };
    auto handle_moved = [&](const livekit::RoomMovedResponse& moved) {
      if (moved.room().sid().empty() || moved.room().name().empty() || moved.token().empty() ||
          moved.participant().sid().empty() || moved.participant().identity() != join.participant().identity())
        throw SDKError(ErrorDomain::protocol, ErrorCode::malformed_message, "Incomplete or inconsistent room migration");
      if (moved.room().sid() == join.room().sid() && moved.room().name() == join.room().name() &&
          moved.participant().sid() == join.participant().sid()) { socket.update_token(moved.token()); return; }
      // Keep transport only when the confirmed publication mapping is unchanged.
      bool keep_transport = connected && !offer_pending && awaiting_tracks.empty() &&
          moved.participant().sid() == join.participant().sid() && moved.participant().tracks_size() == published_info.size();
      std::set<std::string> moved_sids;
      for (const auto& track : moved.participant().tracks())
        keep_transport &= moved_sids.insert(track.sid()).second && published_info.contains(track.sid());
      socket.update_token(moved.token()); socket.bind_participant(moved.participant().sid());
      recovery.expected_room = moved.room().name(); recovery.room_changed = true;
      recovery.remote.clear(); remote_settings.clear(); subscription_overrides.clear(); restored_intents.clear();
      if (video_frames) video_frames->reset();
      if (session && session->audio_output) session->audio_output->advance();
      pending_controls.interrupt("Room migrated before request confirmation; outcome is unknown");
      const auto interrupted = std::make_exception_ptr(SDKError(ErrorDomain::lifecycle,
          ErrorCode::session_closed, "Command interrupted by room migration"));
      for (auto& command : deferred) command->reject(interrupted);
      deferred.clear();
      *join.mutable_room() = moved.room(); *join.mutable_participant() = moved.participant();
      Json participants = Json::array();
      for (const auto& participant : moved.other_participants()) participants.push_back(participant_json(participant));
      event("room-moved", {{"roomInfo", {{"sid", moved.room().sid()}, {"name", moved.room().name()},
          {"metadata", moved.room().metadata()}, {"recording", moved.room().active_recording()}}},
          {"localParticipant", participant_json(moved.participant())}, {"participants", participants},
          {"transportRetained", keep_transport}});
      event.dispatch(); event.rethrow();
      if (stop.stop_requested()) return;
      if (!keep_transport) throw detail::RejoinRequired("Room migration requires transport reconstruction");
      for (const auto& track : moved.participant().tracks()) {
        *published_info.at(track.sid()).mutable_track() = track;
        published_tracks.at(track.sid())->set_enabled(!track.muted());
        if (const auto id = recovery.local_ids.find(track.sid()); id != recovery.local_ids.end())
          recovery.publications.at(id->second).muted = track.muted();
      }
    };
    while (!stop.stop_requested()) {
      try {
      event.dispatch(); event.rethrow();
      if (stop.stop_requested()) break;
      if (Clock::now() >= recovery.session_deadline) break;
      recovery.budget.check();
      pending_controls.poll();
      socket.flush_ice();
      if (playout && !playout->poll()) break;
      if (publisher->failed || subscriber->failed) throw std::runtime_error("WebRTC transport or media failed");
      if (publisher->transport_failed || subscriber->transport_failed)
        throw SDKError(ErrorDomain::transport, ErrorCode::connection_lost, "WebRTC transport failed", true);
      publisher->check_recordings(); subscriber->check_recordings();
      if (options.stats_interval_ms && Clock::now()>=next_stats) {
        publisher->request_stats(); subscriber->request_stats();
        Json nodes = Json::array();
        for (const auto& node : runtime.graph->snapshot()) nodes.push_back({{"id", node.id}, {"name", node.name}, {"kind", node.kind},
          {"format", node.format}, {"inputs", node.inputs}, {"outputs", node.outputs},
          {"errors", node.errors}, {"dropped", node.dropped}, {"generation", node.generation}});
        Json edges = Json::array();
        for (const auto& edge : runtime.graph->connections())
          edges.push_back({{"from", edge.from}, {"to", edge.to}, {"dataType", edge.data_type}});
        event("media-graph-stats", {{"nodes", std::move(nodes)}, {"edges", std::move(edges)}});
        next_stats=Clock::now()+std::chrono::milliseconds(options.stats_interval_ms);
      }
      if (video_frames) video_frames->dispatch();
      for (const auto& pump : pumps) { auto error = pump->error(); if (!error.empty()) throw std::runtime_error(error); }
      if (session) {
        auto queued = session->take_commands();
        deferred.insert(deferred.end(), queued.begin(), queued.end());
        const auto count = deferred.size();
        for (size_t index = 0; index < count; ++index) {
          auto command = deferred.front(); deferred.pop_front();
          try {
            command->expire(Clock::now());
            if (command->done()) continue;
            const auto& a = command->args; const auto& op = command->operation;
            if (op == "subscribe" || op == "settings") {
              bool exists = false; std::lock_guard lock(session->mutex);
              for (const auto& [identity, p] : session->remote) for (const auto& t : p.tracks) if (t.sid == a.at("sid").get<std::string>()) exists = true;
              if (!exists) throw std::logic_error("Remote publication no longer exists");
            }
            livekit::SignalRequest request;
            if (op != "data" && op != "packet" && !command->begin_send()) continue;
            if (op == "publish-microphone") {
              if(!options.audio_playout && !options.microphone && !restore_microphone) throw std::logic_error("Dynamic microphone requires a hardware audio session");
              if(std::any_of(publishing.begin(),publishing.end(),[](const auto&p){return p.second->operation=="publish-microphone";})) throw std::logic_error("Microphone publication is pending");
              if (std::any_of(recovery.publications.begin(),recovery.publications.end(),[](const auto& p){return p.second.microphone;})) throw std::logic_error("Microphone is already published");
              if(runtime.worker->BlockingCall([&]{return runtime.audio_device->enable_capture();})!=0) throw std::runtime_error("Cannot open system microphone");
              need_publisher=true; const auto id=recovery.allocate_id(false);
              auto source=runtime.factory->CreateAudioSource(webrtc::AudioOptions{});
              add_track(runtime.factory->CreateAudioTrack(id,source.get()),nullptr,command->publication);
              publishing[id]=command;dirty=true;continue;
            } else if (op == "publish") {
              if (publishing.size() >= 128) throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted, "Too many unconfirmed publications");
              need_publisher = true;
              const auto id = recovery.allocate_id(command->media.video);
              auto pump = std::make_shared<MediaPump>(command->media, runtime.graph, playout.get(), video_frames.get(), id);
              if (command->media.video) add_track(runtime.factory->CreateVideoTrack(pump->video, id), &command->media, command->publication);
              else add_track(runtime.factory->CreateAudioTrack(id, pump->audio.get()), &command->media, command->publication);
              track_pumps[id] = pump.get(); pumps.push_back(std::move(pump));
              publishing[id] = command; dirty = true; continue;
            } else if (op == "unpublish") {
              auto sid = a.at("sid").get<std::string>();
              if (!published_tracks.contains(sid)) throw std::invalid_argument("Local track is not published");
              remove_track(sid); event.dispatch(); event.rethrow();
            } else if (op == "data" || op == "packet") {
              need_publisher = true;
              auto channel = a.at("reliable").get<bool>() ? reliable : lossy_channel;
              if (!channel || channel->state() != webrtc::DataChannelInterface::kOpen) { deferred.push_back(command); continue; }
              if (!command->begin_send()) continue;
              livekit::DataPacket packet; packet.set_kind(a.at("reliable").get<bool>() ? 0 : 1);
              packet.set_participant_identity(join.participant().identity());
              if (op == "packet") {
                if (!packet.ParseFromString(livekit::sdk::base64_decode(a.at("payload").get<std::string>()))) throw std::invalid_argument("Invalid data packet");
                packet.set_kind(0); packet.set_participant_identity(join.participant().identity());
              } else {
                auto user = packet.mutable_user(); user->set_payload(livekit::sdk::base64_decode(a.at("payload").get<std::string>())); user->set_topic(a.at("topic").get<std::string>());
                for (const auto& target : a.at("destinations")) { packet.add_destination_identities(target.get<std::string>()); user->add_destination_identities(target.get<std::string>()); }
              }
              auto bytes = packet.SerializeAsString();
              if (bytes.size() > (packet.kind() == 0 ? 16384u : 1400u)) throw std::invalid_argument("Encoded data packet exceeds transport limit");
              if (!channel->Send(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()), true))) throw std::runtime_error("Data channel send failed");
            } else if (op == "mute") {
              auto sid = a.at("sid").get<std::string>(); auto track = published_tracks.find(sid);
              if (track == published_tracks.end()) throw std::invalid_argument("Local track is not published");
              const bool muted = a.at("muted"); track->second->set_enabled(!muted);
              request.mutable_mute()->set_sid(sid); request.mutable_mute()->set_muted(muted); send(request);
              if (auto info = published_info.find(sid); info != published_info.end()) info->second.mutable_track()->set_muted(muted);
              if (auto id = recovery.local_ids.find(sid); id != recovery.local_ids.end()) recovery.publications.at(id->second).muted = muted;
              event("track-muted", {{"sid", sid}, {"muted", muted}}); event.dispatch(); event.rethrow();
            } else if (op == "subscribe") {
              request.mutable_subscription()->add_track_sids(a.at("sid").get<std::string>()); request.mutable_subscription()->set_subscribe(a.at("subscribe").get<bool>()); send(request);
              subscription_overrides[a.at("sid").get<std::string>()] = a.at("subscribe").get<bool>();
              remember_remote(a.at("sid").get<std::string>(), a.at("subscribe").get<bool>(), Json::object());
            } else if (op == "settings") {
              auto sid = a.at("sid").get<std::string>();
              auto& current = remote_settings[sid]; if (current.is_null()) current = Json::object(); current.update(a);
              send_settings(sid, current);
              remember_remote(sid, {}, current);
            } else if (op == "permissions") {
              auto policy=detail::parse_permissions(a);
              for(const auto& p:policy.participants)for(const auto& id:p.track_client_ids)
                if(!recovery.publications.contains(id) || std::none_of(recovery.local_ids.begin(),recovery.local_ids.end(),[&](const auto& item){return item.second==id;}))
                  throw std::invalid_argument("Permission source is not a confirmed local publication");
              *request.mutable_subscription_permission()=detail::permissions_wire(policy,recovery.local_ids);
              send(request); recovery.subscription_permissions=std::move(policy);
            } else if (op == "metadata") {
              auto update = request.mutable_update_metadata();
              // Match Go setters: send only the requested field; empty scalar means unchanged.
              if (a.contains("name")) update->set_name(a.at("name").get<std::string>());
              if (a.contains("metadata")) update->set_metadata(a.at("metadata").get<std::string>());
              if (a.contains("attributes")) for (auto it = a.at("attributes").begin(); it != a.at("attributes").end(); ++it) (*update->mutable_attributes())[it.key()] = it.value().get<std::string>();
              if (command->options.wait_for_ack) update->set_request_id(pending_controls.add(command));
              send(request);
              if (command->options.wait_for_ack) continue;
            } else throw std::logic_error("Unknown room command");
            command->resolve();
          } catch (...) { command->reject(std::current_exception()); }
        }
        // Publication timeout requires terminating the session: late server ACKs
        // must not create orphan publications after the application sees failure.
        for (const auto& [id, command] : publishing) if (Clock::now() >= command->deadline) {
          command->expire(Clock::now());
          throw detail::RejoinRequired("Unconfirmed publication expired; transport must be reconciled");
        }
      }
      if(session){
        std::shared_ptr<detail::RpcSession> rpc;
        {std::lock_guard lock(session->mutex);rpc=session->rpc;}
        if(rpc&&rpc->has_outgoing()){
          need_publisher=true;
          if(reliable&&reliable->state()==webrtc::DataChannelInterface::kOpen&&reliable->buffered_amount()<256*1024){
            for(auto packet:rpc->take_packets()){
              packet.set_participant_identity(join.participant().identity());auto bytes=packet.SerializeAsString();
              if(bytes.size()>16384||!reliable->Send(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(bytes.data(),bytes.size()),true)))rpc->send_failed(packet);
            }
          }
        }
      }
      if (need_publisher && dirty && !offer_pending && awaiting_tracks.empty()) {
        livekit::SignalRequest request; *request.mutable_offer() = publisher->local(true); send(request); negotiated = true; dirty = false; offer_pending = true;
      }
      bool primary_ready = join.subscriber_primary() ? subscriber->connected.load() : publisher->connected.load();
      if (recovering && primary_ready && (!need_publisher || publisher->connected) && !offer_pending) {
        recovering = false;
        event("reconnected", {{"mode", "resume"}, {"attempt", socket.recovery_attempt()}});
      }
      if (recovering && Clock::now() >= recovery_deadline)
        throw livekit::sdk::SDKError(livekit::sdk::ErrorDomain::transport, livekit::sdk::ErrorCode::recovery_failed,
                                     "Media transport did not recover after signal resume", true);
      if (!connected && primary_ready && (!need_publisher || publisher->connected)) {
        connected = true; connected_at = Clock::now();
        if (!recovery.ever_connected && options.duration > 0) recovery.session_deadline = connected_at + std::chrono::seconds(options.duration);
        if (recovery.rejoining) event("reconnected", {{"mode", "rejoin"}, {"attempt", recovery.budget.attempts()}});
        else event("connected", {{"room", join.room().name()}});
      }
      if (publisher->connected && negotiated && !offer_pending) { for (auto& pump : pumps) if (started_pumps.insert(pump.get()).second) pump->start(); pumping = true; }
      if (!data_sent && reliable && reliable->state() == webrtc::DataChannelInterface::kOpen) {
        livekit::DataPacket packet; packet.set_participant_identity(join.participant().identity()); packet.mutable_user()->set_payload(options.publish_data); packet.mutable_user()->set_topic(options.topic);
        auto bytes = packet.SerializeAsString();
        if (!reliable->Send(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()), true))) throw std::runtime_error("Data channel send failed");
        data_sent = true; recovery.initial_data_sent = true; event("data-published");
      }
      if (connected && options.duration > 0 && Clock::now() - connected_at >= std::chrono::seconds(options.duration)) break;
      if (connected && pumping && options.exit_after_publish && !pumps.empty() && data_sent && std::all_of(pumps.begin(), pumps.end(), [](const auto& p) { return p->finished(); })) break;
      if (!connected && Clock::now() - joined_at > std::chrono::seconds(options.connect_timeout)) throw SDKError(ErrorDomain::transport, ErrorCode::timeout, "Timed out establishing WebRTC transports or publishing tracks", true);
      if (Clock::now() - last_ping >= ping_interval) {
        livekit::SignalRequest request; request.mutable_ping_req()->set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); send(request); last_ping = Clock::now();
      }
      if (Clock::now() - last_pong > ping_interval + ping_timeout)
        throw livekit::sdk::SDKError(livekit::sdk::ErrorDomain::signaling, livekit::sdk::ErrorCode::timeout, "LiveKit signal heartbeat timed out", true);
      auto message = socket.receive((playout || video_frames) ? 10 : 50); if (!message) continue;
      livekit::SignalResponse response; if (!response.ParseFromString(*message)) throw std::runtime_error("Malformed LiveKit signal message");
      if (response.has_offer()) { subscriber->remote(response.offer()); livekit::SignalRequest request; *request.mutable_answer() = subscriber->local(false); request.mutable_answer()->set_id(response.offer().id()); send(request); }
      else if (response.has_answer()) { publisher->remote(response.answer()); offer_pending = false; }
      else if (response.has_trickle()) (response.trickle().target() == livekit::PUBLISHER ? publisher : subscriber)->ice(response.trickle());
      else if (response.has_track_published()) {
        const auto& cid = response.track_published().cid();
        const auto& sid = response.track_published().track().sid();
        if (!awaiting_tracks.contains(cid)) throw SDKError(ErrorDomain::protocol, ErrorCode::malformed_message, "Unexpected publication confirmation");
        const auto pending_publication = publishing.find(cid);
        if (pending_publication != publishing.end()) pending_publication->second->expire(Clock::now());
        const bool cancelled_publication = pending_publication != publishing.end() && pending_publication->second->done();
        if (auto pending = publishing.find(cid); pending != publishing.end() && !cancelled_publication)
          recovery.publications[cid] = detail::DesiredPublication{pending->second->media, pending->second->publication, pending->second->operation == "publish-microphone"};
        recovery.local_ids[sid] = cid;
        published_info[response.track_published().track().sid()] = response.track_published();
        awaiting_tracks.erase(response.track_published().cid());
        for (const auto& track : tracks) if (track->id() == response.track_published().cid()) published_tracks[response.track_published().track().sid()] = track;
        if (cancelled_publication) {
          remove_track(sid); publishing.erase(cid); continue;
        }
        send_permissions();
        auto info = track_json(response.track_published().track());
        info["clientId"] = cid;
        if (auto desired = recovery.publications.find(cid); desired != recovery.publications.end() && desired->second.muted) {
          livekit::SignalRequest mute; mute.mutable_mute()->set_sid(sid); mute.mutable_mute()->set_muted(true); send(mute);
          info["muted"] = true; published_info[sid].mutable_track()->set_muted(true);
        }
        event("track-published", {{"cid", response.track_published().cid()}, {"sid", response.track_published().track().sid()}, {"trackInfo", info}});
        // Update public state before completing the publication future.
        event.dispatch(); event.rethrow();
        auto pending = publishing.find(response.track_published().cid());
        if (pending != publishing.end()) {
          const bool delivered = pending->second->resolve(info); publishing.erase(pending);
          if (!delivered) remove_track(sid); // cancellation won while the publication callback was dispatched
        }
      }
      else if (response.has_update()) { for (const auto& p : response.update().participants()) {
        event("participant", {{"identity", p.identity()}, {"sid", p.sid()}, {"state", p.state()}, {"participant", participant_json(p)}});
        restore_remote(p);
      } }
      else if (response.has_pong_resp() || response.has_pong()) last_pong = Clock::now();
      else if (response.has_refresh_token()) socket.update_token(response.refresh_token());
      else if (response.has_request_response()) {
        const auto& reply = response.request_response();
        pending_controls.respond(reply.request_id(), reply.reason());
      }
      else if (response.has_room_moved()) handle_moved(response.room_moved());
      else if (response.has_leave()) {
        const auto& leave = response.leave();
        if (options.reconnect.enabled && !detail::terminal_leave(leave.reason()) && (leave.action() != 0 || leave.can_reconnect())) {
          if (leave.has_regions()) socket.update_regions(leave.regions());
          if (leave.action() == 1) throw SDKError(ErrorDomain::signaling, ErrorCode::connection_lost, "Server requested signal resume", true, leave.reason());
          throw detail::RejoinRequired("Server requested full reconnect", leave.reason());
        }
        event("server-leave", {{"reason", leave.reason()}}); break;
      }
      else if (response.has_subscription_response() && response.subscription_response().err()) event("subscription-failed", {{"sid", response.subscription_response().track_sid()}, {"error", response.subscription_response().err()}});
      else if (response.has_subscription_permission_update()) {
        const auto& update=response.subscription_permission_update();
        event("subscription-permission",{{"participantSid",update.participant_sid()},{"trackSid",update.track_sid()},{"allowed",update.allowed()}});
      }
      else if (response.has_room_update()) event("room-update", {{"metadata", response.room_update().room().metadata()}, {"recording", response.room_update().room().active_recording()}});
      else if (response.has_connection_quality()) { for (const auto& q : response.connection_quality().updates()) event("connection-quality", {{"sid", q.participant_sid()}, {"quality", q.quality()}}); }
      else if (response.has_speakers_changed()) { Json speakers = Json::array(); for (const auto& p : response.speakers_changed().speakers()) speakers.push_back({{"sid", p.sid()}, {"level", p.level()}, {"active", p.active()}}); event("speakers", {{"speakers", speakers}}); }
      else if (response.has_mute()) {
        auto track = published_tracks.find(response.mute().sid());
        if (track != published_tracks.end()) track->second->set_enabled(!response.mute().muted());
        if (auto info = published_info.find(response.mute().sid()); info != published_info.end()) info->second.mutable_track()->set_muted(response.mute().muted());
        if (auto id = recovery.local_ids.find(response.mute().sid()); id != recovery.local_ids.end()) recovery.publications.at(id->second).muted = response.mute().muted();
        event("track-muted", {{"sid", response.mute().sid()}, {"muted", response.mute().muted()}});
      }
      else if (response.has_track_unpublished()) remove_track(response.track_unpublished().track_sid());
      } catch (const livekit::sdk::SDKError& error) {
        // Resume only a settled session. Ambiguous publication/SDP operations
        // must not be silently replayed and create an orphan or duplicate track.
        if (dynamic_cast<const detail::RejoinRequired*>(&error) ||
            (error.domain() != livekit::sdk::ErrorDomain::signaling && error.domain() != livekit::sdk::ErrorDomain::transport) || !error.retryable() ||
            !connected || offer_pending || !awaiting_tracks.empty()) throw;
        pending_controls.interrupt("Connection lost before request confirmation; outcome is unknown");
        if (!socket.recover(build_sync, configure_resume, handle_moved)) { if (stop.stop_requested()) break; throw; }
        send_permissions();
        publisher->transport_failed = false; subscriber->transport_failed = false;
        if (negotiated) {
          livekit::SignalRequest restart; *restart.mutable_offer() = publisher->local(true, true);
          send(restart); offer_pending = true;
        }
        recovering = true;
        recovery_deadline = std::min(recovery.budget.deadline(), Clock::now() + std::chrono::seconds(options.connect_timeout));
        last_ping = last_pong = Clock::now();
      }
    }
    cleanup(); event("disconnected");
  } catch (...) { cleanup(); throw; }
}
}
