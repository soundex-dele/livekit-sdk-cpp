#include "engines/rtc/room.hpp"
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
using PC = webrtc::PeerConnectionInterface;
using Clock = std::chrono::steady_clock;
template<class T> T await(std::future<T>& result) {
  if (result.wait_for(std::chrono::seconds(15)) != std::future_status::ready) throw std::runtime_error("WebRTC SDP operation timed out");
  return result.get();
}
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
  Runtime(bool hardware) {
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
      deps.adm = webrtc::make_ref_counted<livekit::sdk::detail::GraphAudioDevice>(deps.adm, graph);
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
    if (signaling) signaling->Stop(); if (worker) worker->Stop(); if (network) network->Stop();
    if (ssl_) { webrtc::CleanupSSL(); ssl_ = false; }
  }
};
class Peer : public webrtc::PeerConnectionObserver, public webrtc::DataChannelObserver {
 public:
  webrtc::scoped_refptr<PC> pc;
  std::atomic<bool> connected{false}, failed{false};
  const livekit::SignalTarget target;
  Peer(Runtime& runtime, SignalSocket& socket, livekit::SignalTarget which, const PC::RTCConfiguration& config, std::filesystem::path record, EventEmitter events, VideoPlayout* playout = nullptr, VideoFrames* frames = nullptr)
      : target(which), event(std::move(events)), socket_(socket), record_(std::move(record)), playout_(playout), frames_(frames), graph_(runtime.graph) {
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
    if (state == PC::PeerConnectionState::kFailed) failed = true;
    event("transport", {{"target", static_cast<int>(target)}, {"state", static_cast<int>(state)}});
  }
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
    try {
      std::string sdp; candidate->ToString(&sdp);
      livekit::SignalRequest request;
      request.mutable_trickle()->set_target(target);
      request.mutable_trickle()->set_candidateinit(Json{{"candidate", sdp}, {"sdpMid", candidate->sdp_mid()}, {"sdpMLineIndex", candidate->sdp_mline_index()}}.dump());
      socket_.send(request.SerializeAsString());
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
      auto sink = std::make_unique<TrackSink>(track, record_, playout_, frames_, sid, graph_);
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
    livekit::DataPacket packet;
    if (!packet.ParseFromArray(buffer.data.data(), static_cast<int>(buffer.data.size()))) return;
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
  void remote(const livekit::SessionDescription& description) {
    webrtc::SdpParseError error;
    auto type = webrtc::SdpTypeFromString(description.type());
    if (!type) throw std::runtime_error("Invalid SDP type");
    auto sdp = webrtc::CreateSessionDescription(*type, description.sdp(), &error);
    if (!sdp) throw std::runtime_error("Invalid remote SDP: " + error.description);
    auto observer = webrtc::make_ref_counted<SetDescription>(); auto future = observer->result.get_future();
    pc->SetRemoteDescription(observer.get(), sdp.release()); await(future);
    remote_set_ = true;
    for (const auto& candidate : pending_ice_) add_ice(candidate);
    pending_ice_.clear();
  }
  livekit::SessionDescription local(bool offer) {
    auto creator = webrtc::make_ref_counted<CreateDescription>(); auto future = creator->result.get_future();
    PC::RTCOfferAnswerOptions options;
    if (offer) pc->CreateOffer(creator.get(), options); else pc->CreateAnswer(creator.get(), options);
    auto sdp = await(future);
    livekit::SessionDescription result; result.set_type(offer ? "offer" : "answer"); sdp->ToString(result.mutable_sdp());
    auto setter = webrtc::make_ref_counted<SetDescription>(); auto set_result = setter->result.get_future();
    pc->SetLocalDescription(setter.get(), sdp.release()); await(set_result); return result;
  }
  void ice(const livekit::TrickleRequest& request) {
    if (request.final() || request.candidateinit().empty()) return;
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
  SignalSocket& socket_;
  std::filesystem::path record_;
  VideoPlayout* playout_ = nullptr;
  VideoFrames* frames_ = nullptr;
  std::shared_ptr<livekit::sdk::detail::RtcGraph> graph_;
  std::mutex mutex_;
  std::vector<std::unique_ptr<TrackSink>> sinks_;
  std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> channels_;
  std::vector<std::string> pending_ice_;
  bool remote_set_ = false;
  std::shared_ptr<std::atomic<bool>> stats_pending_=std::make_shared<std::atomic<bool>>(false);
};
void webrtc_smoke(EventEmitter event) {
  Runtime runtime(false); SignalSocket socket;
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
  SignalSocket socket;
  socket.connect(options.url, options.token, options.subscribe && options.subscribe_tracks.empty(), stop, options.connect_timeout);
  auto start = Clock::now();
  livekit::JoinResponse join;
  while (!stop.stop_requested()) {
    event.dispatch(); event.rethrow();
    auto message = socket.receive(100);
    if (message) {
      livekit::SignalResponse response;
      if (!response.ParseFromString(*message)) throw std::runtime_error("Invalid LiveKit signal response");
      if (response.has_leave()) throw std::runtime_error("Server rejected join, reason=" + std::to_string(response.leave().reason()));
      if (response.has_join()) { join = response.join(); break; }
    }
    if (Clock::now() - start > std::chrono::seconds(options.connect_timeout)) throw std::runtime_error("Timed out waiting for LiveKit JoinResponse");
  }
  if (stop.stop_requested()) return;
  if (!join.alternative_url().empty()) throw std::runtime_error("Server requested alternative URL: " + join.alternative_url());
  if (join.participant().sid().empty()) throw std::runtime_error("Join response has no participant");
  if (!options.room.empty() && join.room().name() != options.room) throw std::runtime_error("Token room does not match the requested room");
  Json roster = Json::array(); for (const auto& p : join.other_participants()) roster.push_back(participant_json(p));
  event("joined", {{"room", join.room().name()}, {"identity", join.participant().identity()}, {"sid", join.participant().sid()},
    {"roomInfo", {{"sid", join.room().sid()}, {"name", join.room().name()}, {"metadata", join.room().metadata()}, {"recording", join.room().active_recording()}}},
    {"localParticipant", participant_json(join.participant())}, {"participants", roster}});
  for (const auto& p : join.other_participants())
    event("participant", {{"identity", p.identity()}, {"sid", p.sid()}, {"state", p.state()}, {"participant", participant_json(p)}, {"initial", true}});
  Runtime runtime(options.audio_playout || options.microphone);
  std::unique_ptr<VideoFrames> video_frames;
  if (options.video_frame_handler) video_frames = std::make_unique<VideoFrames>(options.video_frame_handler);
  std::unique_ptr<VideoPlayout> playout;
  if (options.video_playout) playout = std::make_unique<VideoPlayout>(event);
  PC::RTCConfiguration config; config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  for (const auto& ice : join.ice_servers()) {
    PC::IceServer server; for (const auto& url : ice.urls()) if (!options.disable_turn || (!url.starts_with("turn:") && !url.starts_with("turns:"))) server.urls.push_back(url); server.username = ice.username(); server.password = ice.credential(); if (!server.urls.empty()) config.servers.push_back(std::move(server));
  }
  if (options.relay_only) config.type = PC::kRelay;
  std::unique_ptr<Peer> publisher, subscriber;
  std::vector<std::shared_ptr<MediaPump>> pumps;
  std::vector<webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>> tracks;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> reliable, lossy_channel;
  using Command = livekit::sdk::detail::Command;
  std::map<std::string, std::shared_ptr<Command>> publishing;
  std::map<std::string, Json> remote_settings;
  std::deque<std::shared_ptr<Command>> deferred;
  std::map<std::string, MediaPump*> track_pumps;
  std::set<MediaPump*> started_pumps;
  std::map<std::string, webrtc::scoped_refptr<webrtc::RtpSenderInterface>> senders;
  auto cleanup = [&] {
    runtime.graph->close();
    auto closed = std::make_exception_ptr(std::runtime_error("Room disconnected before command completed"));
    for (auto& [id, command] : publishing) command->reject(closed);
    for (auto& command : deferred) command->reject(closed);
    for (auto& pump : pumps) pump->stop();
    try { livekit::SignalRequest leave; leave.mutable_leave(); socket.send(leave.SerializeAsString()); } catch (...) {}
    socket.close();
    runtime.signaling->BlockingCall([&] { reliable = nullptr; lossy_channel = nullptr; senders.clear(); publisher.reset(); subscriber.reset(); tracks.clear(); });
    for (const auto& pump : pumps) event("publish-summary", {{"kind", pump->spec.video ? "video" : "audio"}, {"frames", pump->frames.load()}});
    pumps.clear();
  };
  try {
    publisher = std::make_unique<Peer>(runtime, socket, livekit::PUBLISHER, config, std::filesystem::path{}, event);
    subscriber = std::make_unique<Peer>(runtime, socket, livekit::SUBSCRIBER, config, options.record, event, playout.get(), video_frames.get());
    std::set<std::string> awaiting_tracks;
    std::map<std::string, webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>> published_tracks;
    auto send = [&](const livekit::SignalRequest& request) { socket.send(request.SerializeAsString()); };
    auto add_track = [&](webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, const MediaSpec* spec, const livekit::sdk::TrackPublishOptions& publication = {}) {
      webrtc::RtpTransceiverInit init; init.direction = webrtc::RtpTransceiverDirection::kSendOnly; init.stream_ids = {publication.stream.empty() ? "livekit-sdk" : publication.stream};
      webrtc::RtpEncodingParameters encoding; encoding.max_bitrate_bps = track->kind() == "video" ? 2000000 : 64000; init.send_encodings.push_back(encoding);
      auto result = publisher->pc->AddTransceiver(track, init);
      if (!result.ok()) throw std::runtime_error(result.error().message());
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
      const auto default_source = spec && spec->capture == livekit::sdk::CaptureKind::screen ? livekit::SCREEN_SHARE :
          track->kind() == "video" ? livekit::CAMERA : livekit::MICROPHONE;
      add->set_source(publication.source == livekit::sdk::TrackSource::unknown ? default_source : static_cast<livekit::TrackSource>(publication.source));
      if (spec && spec->video) { add->set_width(spec->width); add->set_height(spec->height); auto layer = add->add_layers(); layer->set_quality(2); layer->set_width(spec->width); layer->set_height(spec->height); layer->set_bitrate(2000000); }
      send(request);
    };
    int counter = 0;
    for (const auto& spec : options.media) {
      auto id = std::string(spec.video ? "video-" : "audio-") + std::to_string(++counter);
      auto pump = std::make_shared<MediaPump>(spec, runtime.graph, playout.get(), video_frames.get(), id);
      if (spec.video) add_track(runtime.factory->CreateVideoTrack(pump->video, id), &spec);
      else add_track(runtime.factory->CreateAudioTrack(id, pump->audio.get()), &spec);
      track_pumps[id] = pump.get(); pumps.push_back(std::move(pump));
    }
    if (options.microphone) {
      auto source = runtime.factory->CreateAudioSource(webrtc::AudioOptions{});
      add_track(runtime.factory->CreateAudioTrack("microphone", source.get()), nullptr);
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
    if (!options.subscribe_tracks.empty()) {
      livekit::SignalRequest request; auto* subscription = request.mutable_subscription(); subscription->set_subscribe(true);
      for (const auto& sid : options.subscribe_tracks) subscription->add_track_sids(sid); send(request);
    }
    bool negotiated = false, dirty = true, offer_pending = false, pumping = false, connected = false, data_sent = options.publish_data.empty();
    auto remove_track = [&](const std::string& sid) {
      auto found = published_tracks.find(sid); if (found == published_tracks.end()) return;
      auto cid = found->second->id(); auto sender = senders.find(cid);
      if (sender != senders.end()) { auto error = publisher->pc->RemoveTrackOrError(sender->second); if (!error.ok()) throw std::runtime_error(error.message()); senders.erase(sender); }
      auto pump = track_pumps.find(cid);
      if (pump != track_pumps.end()) { pump->second->stop(); started_pumps.erase(pump->second); std::erase_if(pumps, [&](const auto& p) { return p.get() == pump->second; }); track_pumps.erase(pump); }
      std::erase_if(tracks, [&](const auto& t) { return t->id() == cid; });
      published_tracks.erase(found); dirty = true; event("track-unpublished", {{"sid", sid}});
    };
    auto joined_at = Clock::now(), last_ping = joined_at, last_pong = joined_at, connected_at = joined_at;
    auto next_stats=joined_at;
    auto ping_interval = std::chrono::seconds(join.ping_interval() > 0 ? join.ping_interval() : 5);
    auto ping_timeout = std::chrono::seconds(join.ping_timeout() > 0 ? join.ping_timeout() : 15);
    while (!stop.stop_requested()) {
      event.dispatch(); event.rethrow();
      if (stop.stop_requested()) break;
      if (playout && !playout->poll()) break;
      if (publisher->failed || subscriber->failed) throw std::runtime_error("WebRTC transport or media failed");
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
            if (Clock::now() > command->deadline) throw std::runtime_error("Room command timed out");
            const auto& a = command->args; const auto& op = command->operation;
            if (op == "subscribe" || op == "settings") {
              bool exists = false; std::lock_guard lock(session->mutex);
              for (const auto& [identity, p] : session->remote) for (const auto& t : p.tracks) if (t.sid == a.at("sid").get<std::string>()) exists = true;
              if (!exists) throw std::logic_error("Remote publication no longer exists");
            }
            livekit::SignalRequest request;
            if (op == "publish") {
              need_publisher = true;
              const auto id = std::string(command->media.video ? "video-" : "audio-") + std::to_string(++counter);
              auto pump = std::make_shared<MediaPump>(command->media, runtime.graph, playout.get(), video_frames.get(), id);
              if (command->media.video) add_track(runtime.factory->CreateVideoTrack(pump->video, id), &command->media, command->publication);
              else add_track(runtime.factory->CreateAudioTrack(id, pump->audio.get()), &command->media, command->publication);
              track_pumps[id] = pump.get(); pumps.push_back(std::move(pump));
              publishing[id] = command; dirty = true; continue;
            } else if (op == "unpublish") {
              auto sid = a.at("sid").get<std::string>();
              if (!published_tracks.contains(sid)) throw std::invalid_argument("Local track is not published");
              remove_track(sid); event.dispatch(); event.rethrow();
            } else if (op == "data") {
              need_publisher = true;
              auto channel = a.at("reliable").get<bool>() ? reliable : lossy_channel;
              if (!channel || channel->state() != webrtc::DataChannelInterface::kOpen) { deferred.push_back(command); continue; }
              livekit::DataPacket packet; packet.set_kind(a.at("reliable").get<bool>() ? 0 : 1);
              packet.set_participant_identity(join.participant().identity());
              auto user = packet.mutable_user(); user->set_payload(livekit::sdk::base64_decode(a.at("payload").get<std::string>())); user->set_topic(a.at("topic").get<std::string>());
              for (const auto& target : a.at("destinations")) { packet.add_destination_identities(target.get<std::string>()); user->add_destination_identities(target.get<std::string>()); }
              auto bytes = packet.SerializeAsString();
              if (bytes.size() > (packet.kind() == 0 ? 16384u : 1400u)) throw std::invalid_argument("Encoded data packet exceeds transport limit");
              if (!channel->Send(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()), true))) throw std::runtime_error("Data channel send failed");
            } else if (op == "mute") {
              auto sid = a.at("sid").get<std::string>(); auto track = published_tracks.find(sid);
              if (track == published_tracks.end()) throw std::invalid_argument("Local track is not published");
              const bool muted = a.at("muted"); track->second->set_enabled(!muted);
              request.mutable_mute()->set_sid(sid); request.mutable_mute()->set_muted(muted); send(request);
              event("track-muted", {{"sid", sid}, {"muted", muted}}); event.dispatch(); event.rethrow();
            } else if (op == "subscribe") {
              request.mutable_subscription()->add_track_sids(a.at("sid").get<std::string>()); request.mutable_subscription()->set_subscribe(a.at("subscribe").get<bool>()); send(request);
            } else if (op == "settings") {
              auto sid = a.at("sid").get<std::string>();
              auto& current = remote_settings[sid]; if (current.is_null()) current = Json::object(); current.update(a);
              auto settings = request.mutable_track_setting(); settings->add_track_sids(sid);
              settings->set_disabled(current.value("disabled", false)); settings->set_quality(2);
              settings->set_width(current.value("width", 0)); settings->set_height(current.value("height", 0)); send(request);
            } else if (op == "metadata") {
              auto update = request.mutable_update_metadata();
              // Match Go setters: send only the requested field; empty scalar means unchanged.
              if (a.contains("name")) update->set_name(a.at("name").get<std::string>());
              if (a.contains("metadata")) update->set_metadata(a.at("metadata").get<std::string>());
              if (a.contains("attributes")) for (auto it = a.at("attributes").begin(); it != a.at("attributes").end(); ++it) (*update->mutable_attributes())[it.key()] = it.value().get<std::string>();
              send(request);
            } else throw std::logic_error("Unknown room command");
            command->resolve();
          } catch (...) { command->reject(std::current_exception()); }
        }
        // Publication timeout requires terminating the session: late server ACKs
        // must not create orphan publications after the application sees failure.
        for (const auto& [id, command] : publishing) if (Clock::now() > command->deadline) throw std::runtime_error("Track publication timed out");
      }
      if (need_publisher && dirty && !offer_pending && awaiting_tracks.empty()) {
        livekit::SignalRequest request; *request.mutable_offer() = publisher->local(true); send(request); negotiated = true; dirty = false; offer_pending = true;
      }
      bool primary_ready = join.subscriber_primary() ? subscriber->connected.load() : publisher->connected.load();
      if (!connected && primary_ready && (!need_publisher || publisher->connected)) {
        connected = true; connected_at = Clock::now(); event("connected", {{"room", join.room().name()}});
      }
      if (publisher->connected && negotiated && !offer_pending) { for (auto& pump : pumps) if (started_pumps.insert(pump.get()).second) pump->start(); pumping = true; }
      if (!data_sent && reliable && reliable->state() == webrtc::DataChannelInterface::kOpen) {
        livekit::DataPacket packet; packet.set_participant_identity(join.participant().identity()); packet.mutable_user()->set_payload(options.publish_data); packet.mutable_user()->set_topic(options.topic);
        auto bytes = packet.SerializeAsString();
        if (!reliable->Send(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()), true))) throw std::runtime_error("Data channel send failed");
        data_sent = true; event("data-published");
      }
      if (connected && options.duration > 0 && Clock::now() - connected_at >= std::chrono::seconds(options.duration)) break;
      if (connected && pumping && options.exit_after_publish && !pumps.empty() && data_sent && std::all_of(pumps.begin(), pumps.end(), [](const auto& p) { return p->finished(); })) break;
      if (!connected && Clock::now() - joined_at > std::chrono::seconds(options.connect_timeout)) throw std::runtime_error("Timed out establishing WebRTC transports or publishing tracks");
      if (Clock::now() - last_ping >= ping_interval) {
        livekit::SignalRequest request; request.mutable_ping_req()->set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); send(request); last_ping = Clock::now();
      }
      if (Clock::now() - last_pong > ping_interval + ping_timeout) throw std::runtime_error("LiveKit signal heartbeat timed out");
      auto message = socket.receive((playout || video_frames) ? 10 : 50); if (!message) continue;
      livekit::SignalResponse response; if (!response.ParseFromString(*message)) throw std::runtime_error("Malformed LiveKit signal message");
      if (response.has_offer()) { subscriber->remote(response.offer()); livekit::SignalRequest request; *request.mutable_answer() = subscriber->local(false); request.mutable_answer()->set_id(response.offer().id()); send(request); }
      else if (response.has_answer()) { publisher->remote(response.answer()); offer_pending = false; }
      else if (response.has_trickle()) (response.trickle().target() == livekit::PUBLISHER ? publisher : subscriber)->ice(response.trickle());
      else if (response.has_track_published()) {
        awaiting_tracks.erase(response.track_published().cid());
        for (const auto& track : tracks) if (track->id() == response.track_published().cid()) published_tracks[response.track_published().track().sid()] = track;
        auto info = track_json(response.track_published().track());
        event("track-published", {{"cid", response.track_published().cid()}, {"sid", response.track_published().track().sid()}, {"trackInfo", info}});
        // Update public state before completing the publication future.
        event.dispatch(); event.rethrow();
        auto pending = publishing.find(response.track_published().cid());
        if (pending != publishing.end()) { pending->second->resolve(info); publishing.erase(pending); }
      }
      else if (response.has_update()) { for (const auto& p : response.update().participants()) event("participant", {{"identity", p.identity()}, {"sid", p.sid()}, {"state", p.state()}, {"participant", participant_json(p)}}); }
      else if (response.has_pong_resp() || response.has_pong()) last_pong = Clock::now();
      else if (response.has_leave()) { event("server-leave", {{"reason", response.leave().reason()}}); break; }
      else if (response.has_subscription_response() && response.subscription_response().err()) event("subscription-failed", {{"sid", response.subscription_response().track_sid()}, {"error", response.subscription_response().err()}});
      else if (response.has_room_update()) event("room-update", {{"metadata", response.room_update().room().metadata()}, {"recording", response.room_update().room().active_recording()}});
      else if (response.has_connection_quality()) { for (const auto& q : response.connection_quality().updates()) event("connection-quality", {{"sid", q.participant_sid()}, {"quality", q.quality()}}); }
      else if (response.has_speakers_changed()) { Json speakers = Json::array(); for (const auto& p : response.speakers_changed().speakers()) speakers.push_back({{"sid", p.sid()}, {"level", p.level()}, {"active", p.active()}}); event("speakers", {{"speakers", speakers}}); }
      else if (response.has_mute()) {
        auto track = published_tracks.find(response.mute().sid());
        if (track != published_tracks.end()) track->second->set_enabled(!response.mute().muted());
        event("track-muted", {{"sid", response.mute().sid()}, {"muted", response.mute().muted()}});
      }
      else if (response.has_track_unpublished()) remove_track(response.track_unpublished().track_sid());
    }
    cleanup(); event("disconnected");
  } catch (...) { cleanup(); throw; }
}
}
