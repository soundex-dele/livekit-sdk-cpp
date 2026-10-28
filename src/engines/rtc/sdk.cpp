#include "livekit/livekit.hpp"
#include "engines/rtc/room.hpp"
#include "engines/rtc/session.hpp"
#include "engines/rtc/reconnect_policy.hpp"
#include "engines/rtc/subscription_permissions.hpp"
#include <set>
#include <algorithm>
#include <random>

namespace livekit::sdk {
SdkCapabilities sdk_capabilities() {
  SdkCapabilities result;result.version=version;result.signal_protocols={12,15,17};
  for(auto feature:{ClientFeature::signal_resume,ClientFeature::full_rejoin,ClientFeature::initial_signal_failover,
      ClientFeature::token_provider,ClientFeature::i420_input,ClientFeature::rgba_input,ClientFeature::pcm_input,
      ClientFeature::video_pull,ClientFeature::audio_pull,ClientFeature::subscription_permissions,
      ClientFeature::receive_quality,ClientFeature::dtmf,ClientFeature::transcription,ClientFeature::rpc})
    result.features[feature]=CapabilitySupport::supported;
  for(auto feature:{ClientFeature::text_stream,ClientFeature::byte_stream,ClientFeature::data_track,
      ClientFeature::media_e2ee,ClientFeature::data_e2ee,ClientFeature::simulcast,ClientFeature::single_peer_connection,
      ClientFeature::encoded_media,ClientFeature::rtp_access,ClientFeature::control_executor})
    result.features[feature]=CapabilitySupport::supported;
  return result;
}
namespace {
void validate(const RoomOptions& o) {
  if(o.control_callback_capacity<1||o.control_callback_capacity>4096)throw std::invalid_argument("Invalid control callback capacity");
  if(o.subscription_permissions)detail::validate_permissions(*o.subscription_permissions);
  detail::validate_reconnect(o.reconnect);
  endpoint(o.url, true);
  detail::signal_endpoint(o.url);
  for (const auto& url : o.regions.trusted_endpoints) detail::trusted_destination(o.url, url, o.regions);
  for (const auto& url : o.regions.fallback_endpoints) detail::trusted_destination(o.url,url,o.regions);
  if(o.regions.fallback_endpoints.size()>32 || o.regions.endpoint_timeout<std::chrono::milliseconds(100) ||
      o.regions.endpoint_timeout>std::chrono::minutes(5))throw std::invalid_argument("Invalid initial region failover options");
  if (o.regions.max_redirects > 10 || o.regions.discovery_timeout <= std::chrono::milliseconds::zero() ||
      o.regions.discovery_timeout > std::chrono::seconds(30)) throw std::invalid_argument("Invalid region discovery options");
  if (o.token.empty() && !o.token_provider) throw std::invalid_argument("Token or token provider is required");
  if (o.token_timeout <= std::chrono::milliseconds::zero() || o.token_timeout > std::chrono::minutes(5))
    throw std::invalid_argument("Invalid token provider timeout");
  if (o.connect_timeout <= 0 || o.duration < 0) throw std::invalid_argument("Invalid room timeout/duration");
  if (o.signal_protocol != 12 && o.signal_protocol != 15 && o.signal_protocol != 17) throw std::invalid_argument("Supported signal protocols are 12, 15 and 17");
  if(o.single_peer_connection&&o.signal_protocol!=17)throw std::invalid_argument("Single PC requires protocol 17");
  if (o.stats_interval_ms && (o.stats_interval_ms < 200 || o.stats_interval_ms > 60000))
    throw std::invalid_argument("stats_interval_ms must be 0 or 200..60000");
  if (o.relay_only && o.disable_turn) throw std::invalid_argument("relay_only conflicts with disable_turn");
  switch (o.media_transport) {
    case MediaTransport::automatic: break;
    case MediaTransport::udp_only:
    case MediaTransport::tcp_only:
      if (o.relay_only) throw std::invalid_argument("relay_only conflicts with direct media_transport");
      break;
    default: throw std::invalid_argument("Invalid media_transport");
  }
  const bool local_preview = std::any_of(o.media.begin(),o.media.end(),[](const auto& m){return m.video&&m.local_preview;});
  if (o.video_frame_handler && o.video_frame_stream) throw std::invalid_argument("Choose video_frame_handler or video_frame_stream");
  if (!o.subscribe) {
    if (o.audio_frame_stream) throw std::invalid_argument("Remote PCM output requires subscriptions");
    if(!o.record.empty())throw std::invalid_argument("Recording requires subscriptions");
    if(o.video_playout||o.audio_playout)throw std::invalid_argument("Remote playout requires subscriptions");
    if((o.video_frame_handler||o.video_frame_stream)&&!local_preview)throw std::invalid_argument("Frame output requires subscriptions or a local preview source");
    if(!o.subscribe_tracks.empty())throw std::invalid_argument("Track selection requires subscriptions");
  }
  if (!std::set<std::string>{"vp8", "vp9", "h264", "h265", "av1"}.contains(o.codec)) throw std::invalid_argument("Unsupported video codec");
  if (o.exit_after_publish && (o.media.empty() || o.microphone)) throw std::invalid_argument("exit_after_publish requires finite media sources");
  for (const auto& m : o.media) {
    detail::validate_media(m);
    if (o.exit_after_publish && (m.demo || m.loop)) throw std::invalid_argument("exit_after_publish requires finite media sources");
  }
}
void prepare(const std::shared_ptr<detail::RoomState>& s, const RoomOptions& o) {
  validate(o);
  std::lock_guard lock(s->mutex);
  if (s->running) throw std::logic_error("Room session is already active");
  auto video_output = o.video_frame_stream ? std::make_shared<detail::VideoFrameOutput>(o.video_frame_stream) : nullptr;
  auto audio_output = o.audio_frame_stream ? std::make_shared<detail::AudioFrameOutput>(o.audio_frame_stream) : nullptr;
  s->encoded_output=o.encoded_frame_stream?std::make_shared<detail::EncodedOutput>(o.encoded_frame_stream):nullptr;
  s->video_output = std::move(video_output); s->audio_output = std::move(audio_output);
  if (s->video_output) s->video_output->advance();
  if (s->audio_output) s->audio_output->advance();
        if(s->encoded_output)s->encoded_output->advance();
  s->running = true; ++s->generation; s->stop = std::stop_source{};
  s->control=o.control_callback_thread?std::make_shared<detail::ControlExecutor>(o.control_callback_capacity,[stop=s->stop]() mutable {stop.request_stop();}):nullptr;
  s->connection = ConnectionState::connecting; s->room = {}; s->local = {}; s->remote.clear(); s->subscriptions.clear();
  s->reason = 0; s->failure = {}; s->error.clear(); s->runner = {}; s->command_timeout = o.connect_timeout;
  s->signal_protocol = o.signal_protocol;
  s->data_tracks->configure(o.encryption);
  s->capabilities = {};
  s->recovery = detail::RecoveryState{};
  s->recovery.budget = detail::RecoveryBudget(o.reconnect);
  s->recovery.initial_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(o.connect_timeout);
  s->recovery.token = o.token;
  s->recovery.expected_room = o.room;
  s->recovery.subscription_permissions=o.subscription_permissions;
  for (const auto& media : o.media) {
    const auto id = s->recovery.allocate_id(media.video);
    s->recovery.publications.emplace(id, detail::DesiredPublication{media, {}, false, false});
  }
  if (o.microphone) s->recovery.publications.emplace("microphone", detail::DesiredPublication{{}, {}, true, false});
}
void execute(const std::shared_ptr<detail::RoomState>& s, const RoomOptions& o, EventHandler handler, std::stop_token external) noexcept {
  { std::lock_guard lock(s->mutex); s->runner = std::this_thread::get_id(); }
  std::stop_callback cancel(external, [s] { s->stop.request_stop(); if (s->video_output) s->video_output->close(); if (s->audio_output) s->audio_output->close(); if(s->encoded_output)s->encoded_output->close(); });
  std::exception_ptr failure;
  lk::EventEmitter events([s, log=o.log_sink, handler = std::move(handler)](const Json& event) {
    detail::process_event(s, event); if(log)s->callback([log,event]{log(event);}); if (handler) s->callback([handler,event]{handler(event);});
  });
  try {
    std::mt19937 random(std::random_device{}());
    std::uniform_real_distribution<double> unit(0, 1);
    while (!s->stop.stop_requested()) {
      if (std::chrono::steady_clock::now() >= s->recovery.session_deadline) break;
      try {
        lk::join_room(o, s->stop.get_token(), events, s);
        events.dispatch(); events.rethrow();
        break;
      } catch (const SDKError& error) {
        events.dispatch(); events.rethrow(); // user callback failures are terminal
        if (s->stop.stop_requested()) break;
        auto& recovery = s->recovery;
        if (std::chrono::steady_clock::now() >= recovery.session_deadline) break;
        if(!recovery.ever_connected&&o.regions.initial_failover&&error.retryable()&&(error.domain()==ErrorDomain::transport||error.domain()==ErrorDomain::signaling)&&std::chrono::steady_clock::now()<recovery.initial_deadline){
          auto& regions=recovery.regions;const auto previous=regions.active_url;regions.attempted.insert(previous);regions.select();
          if(regions.active_url!=previous){
            if(s->video_output)s->video_output->advance();if(s->audio_output)s->audio_output->advance();if(s->encoded_output)s->encoded_output->advance();
            {std::lock_guard lock(s->mutex);++s->generation;s->remote.clear();s->local={};s->room={};s->subscriptions.clear();s->capabilities={};recovery.local_ids.clear();}
            s->data_tracks->disconnect();
            events("initial-region-failover",{{"endpoint",regions.active_url},{"phase","media"}});continue;
          }
        }
        if (!o.reconnect.enabled || !recovery.ever_connected || !error.retryable() ||
            (error.domain() != ErrorDomain::signaling && error.domain() != ErrorDomain::transport)) throw;
        recovery.validate_sources();
        const auto attempt = recovery.budget.next_attempt();
        if (s->video_output) s->video_output->advance();
        if (s->audio_output) s->audio_output->advance();
        if(s->encoded_output)s->encoded_output->advance();
        {
          std::lock_guard lock(s->mutex);
          if (!recovery.rejoining) recovery.local_snapshot = s->local;
          recovery.rejoining = true;
          ++s->generation; // complete rejoin invalidates all old public handles
          s->connection = ConnectionState::reconnecting;
          s->remote.clear(); s->subscriptions.clear(); s->local = {}; s->room = {}; s->reason = 0;
          s->capabilities = {};
          recovery.local_ids.clear();
        }
        const auto interrupted = std::make_exception_ptr(SDKError(ErrorDomain::lifecycle,
            ErrorCode::session_closed, "Command interrupted by full rejoin"));
        for (auto& command : s->take_commands()) command->reject(interrupted);
        events("reconnecting", {{"mode", "rejoin"}, {"attempt", attempt}});
        events.dispatch(); events.rethrow();
        if (s->stop.stop_requested()) break;
        const auto wake = std::min({recovery.budget.deadline(), recovery.session_deadline,
            std::chrono::steady_clock::now() + detail::reconnect_delay(o.reconnect, attempt, unit(random))});
        if (!detail::wait_reconnect(s->stop.get_token(), wake)) break;
        recovery.budget.check();
      }
    }
  }
  catch (...) { failure = std::current_exception(); events.dispatch(); }
  // Stop enqueueing before draining commands, including commands from callbacks.
  s->stop.request_stop();
  std::shared_ptr<detail::RpcSession> rpc;
  {std::lock_guard lock(s->mutex);rpc=s->rpc;}
  if(rpc)rpc->disconnect();
  std::shared_ptr<detail::StreamSession> streams;
  {std::lock_guard lock(s->mutex);streams=s->streams;}
  if(streams)streams->disconnect();
  s->data_tracks->disconnect();
  if(o.packet_stream)o.packet_stream->close();
  if (s->video_output) s->video_output->close();
  if (s->audio_output) s->audio_output->close(); if(s->encoded_output)s->encoded_output->close();
  for (auto& command : s->take_commands()) command->interrupt(ErrorCode::session_closed, "Room disconnected before command completed");
  int reason;
  { std::lock_guard lock(s->mutex); s->connection = ConnectionState::disconnected; if (!s->reason) s->reason = failure ? 9 : 1; reason = s->reason; }
  try { if (s->callbacks.on_disconnected) s->callback([fn=s->callbacks.on_disconnected,reason]{fn(reason);}); if(s->control)s->control->drain(); }
  catch (...) { if (!failure) failure = std::current_exception(); }
  {
    std::lock_guard lock(s->mutex); s->failure = failure;
    if (failure) { try { std::rethrow_exception(failure); } catch (const std::exception& e) { s->error = e.what(); } catch (...) { s->error = "Unknown session error"; } }
    s->running = false; s->runner = {}; s->cv.notify_all();
  }
}
}
struct Room::Impl {
  std::shared_ptr<detail::RoomState> state = std::make_shared<detail::RoomState>();
  std::mutex lifecycle;
  std::thread worker;
};
Room::Room(RoomCallback callbacks) : impl_(std::make_unique<Impl>()) { impl_->state->callbacks = std::move(callbacks); }
SessionCapabilities Room::capabilities() const {
  auto state=impl_->state;std::lock_guard lock(state->mutex);
  auto result=state->connection==ConnectionState::disconnected?SessionCapabilities{}:state->capabilities;
  result.generation=state->generation;result.connected=state->connection==ConnectionState::connected;return result;
}
void Room::register_rpc_method(const std::string& method,RpcHandler handler){
  auto state=impl_->state;std::lock_guard lock(state->mutex);state->rpc_session()->register_method(method,std::move(handler));
}
void Room::unregister_rpc_method(const std::string& method){
  auto state=impl_->state;std::lock_guard lock(state->mutex);if(state->rpc)state->rpc->unregister_method(method);
}
void Room::register_text_stream_handler(const std::string& topic,StreamHandler handler){auto s=impl_->state;std::lock_guard lock(s->mutex);s->stream_session()->register_handler(true,topic,std::move(handler));}
void Room::register_byte_stream_handler(const std::string& topic,StreamHandler handler){auto s=impl_->state;std::lock_guard lock(s->mutex);s->stream_session()->register_handler(false,topic,std::move(handler));}
void Room::unregister_text_stream_handler(const std::string& topic){auto s=impl_->state;std::lock_guard lock(s->mutex);if(s->streams)s->streams->unregister_handler(true,topic);}
void Room::unregister_byte_stream_handler(const std::string& topic){auto s=impl_->state;std::lock_guard lock(s->mutex);if(s->streams)s->streams->unregister_handler(false,topic);}
std::vector<LocalDataTrack> Room::local_data_tracks() const{return impl_->state->data_tracks->local_tracks();}
std::vector<RemoteDataTrack> Room::remote_data_tracks() const{return impl_->state->data_tracks->remote_tracks();}
Room::~Room() {
  request_stop();
  std::thread worker;
  { std::lock_guard lock(impl_->lifecycle); worker = std::move(impl_->worker); }
  if (worker.joinable()) { if (worker.get_id() == std::this_thread::get_id() || (impl_->state->control && impl_->state->control->current())) worker.detach(); else worker.join(); }
}
void Room::request_stop() noexcept {
  std::stop_source stop;
  std::shared_ptr<detail::VideoFrameOutput> video;
  std::shared_ptr<detail::AudioFrameOutput> audio;
  std::shared_ptr<detail::EncodedOutput> encoded;
  { std::lock_guard lock(impl_->state->mutex); stop = impl_->state->stop; video = impl_->state->video_output; audio = impl_->state->audio_output; encoded = impl_->state->encoded_output; }
  stop.request_stop();
  if (video) video->close();
  if (audio) audio->close();
  if (encoded) encoded->close();
}
void Room::run(const RoomOptions& options, EventHandler handler, std::stop_token external) {
  auto s = impl_->state;
  { std::lock_guard lock(impl_->lifecycle); if (impl_->worker.joinable()) throw std::logic_error("Call wait() before reusing an asynchronous Room"); prepare(s, options); }
  execute(s, options, std::move(handler), external);
  std::exception_ptr error; { std::lock_guard lock(s->mutex); error = s->failure; }
  if (error) std::rethrow_exception(error);
}
void Room::join_with_token(const std::string& url, const std::string& token, RoomOptions options, std::stop_token stop) {
  options.url = url; options.token = token;
  if (options.video_playout) throw std::invalid_argument("Video playout requires run() on the main thread");
  auto s = impl_->state;
  {
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->worker.joinable()) throw std::logic_error("Call disconnect() or wait() before joining again");
    prepare(s, options);
    try { impl_->worker = std::thread([s, options, stop] { execute(s, options, {}, stop); }); }
    catch (...) { std::lock_guard state_lock(s->mutex); s->running = false; s->connection = ConnectionState::disconnected;
      if (s->video_output) s->video_output->close(); if (s->audio_output) s->audio_output->close(); if(s->encoded_output)s->encoded_output->close(); throw; }
  }
  std::unique_lock lock(s->mutex);
  s->cv.wait(lock, [&] { return s->connection == ConnectionState::connected || !s->running; });
  if (!s->running) { auto failure = s->failure; lock.unlock(); disconnect(); if (failure) std::rethrow_exception(failure); throw std::runtime_error("Room join cancelled"); }
}
void Room::join(const std::string& url, const ConnectInfo& info, RoomOptions options) {
  if (info.room_name.empty() || info.participant_identity.empty()) throw std::invalid_argument("Room name and participant identity are required");
  auto token = AccessToken(info.api_key, info.api_secret).set_identity(info.participant_identity)
    .set_name(info.participant_name).set_metadata(info.participant_metadata).set_attributes(info.participant_attributes)
    .set_video_grant({{"roomJoin", true}, {"room", info.room_name}}).to_jwt();
  options.room = info.room_name; join_with_token(url, token, std::move(options));
}
void Room::disconnect() {
  request_stop();
  std::thread worker;
  { std::lock_guard lock(impl_->lifecycle); if (impl_->worker.get_id() == std::this_thread::get_id() || (impl_->state->control&&impl_->state->control->current())) return; worker = std::move(impl_->worker); }
  if (worker.joinable()) worker.join();
}
void Room::wait() {
  auto s = impl_->state;
  { std::unique_lock lock(s->mutex); if (s->runner == std::this_thread::get_id() || (s->control&&s->control->current())) throw std::logic_error("Cannot wait on the callback thread"); s->cv.wait(lock, [&] { return !s->running; }); }
  std::thread worker;
  { std::lock_guard lock(impl_->lifecycle); worker = std::move(impl_->worker); }
  if (worker.joinable()) worker.join();
  std::exception_ptr failure; { std::lock_guard lock(s->mutex); failure = s->failure; }
  if (failure) std::rethrow_exception(failure);
}
ConnectionState Room::connection_state() const { std::lock_guard lock(impl_->state->mutex); return impl_->state->connection; }
RoomInfo Room::info() const { std::lock_guard lock(impl_->state->mutex); return impl_->state->room; }
int Room::disconnect_reason() const { std::lock_guard lock(impl_->state->mutex); return impl_->state->reason; }
std::string Room::last_error() const { std::lock_guard lock(impl_->state->mutex); return impl_->state->error; }
LocalParticipant Room::local_participant() const { auto s = impl_->state; std::lock_guard lock(s->mutex); return {s, s->generation, s->local, true}; }
std::vector<RemoteParticipant> Room::remote_participants() const {
  auto s = impl_->state; std::lock_guard lock(s->mutex); std::vector<RemoteParticipant> result;
  for (auto& [id, p] : s->remote) result.emplace_back(s, s->generation, p, false); return result;
}
std::optional<RemoteParticipant> Room::participant_by_identity(const std::string& identity) const {
  auto s = impl_->state; std::lock_guard lock(s->mutex); auto it = s->remote.find(identity);
  if (it == s->remote.end()) return {}; return RemoteParticipant(s, s->generation, it->second, false);
}
std::optional<RemoteParticipant> Room::participant_by_sid(const std::string& sid) const {
  auto s = impl_->state; std::lock_guard lock(s->mutex);
  for (auto& [id, p] : s->remote) if (p.sid == sid) return RemoteParticipant(s, s->generation, p, false); return {};
}
std::unique_ptr<Room> connect_to_room_with_token(const std::string& url, const std::string& token, RoomCallback callbacks, RoomOptions options) {
  auto room = std::make_unique<Room>(std::move(callbacks)); room->join_with_token(url, token, std::move(options)); return room;
}
std::unique_ptr<Room> connect_to_room(const std::string& url, const ConnectInfo& info, RoomCallback callbacks, RoomOptions options) {
  auto room = std::make_unique<Room>(std::move(callbacks)); room->join(url, info, std::move(options)); return room;
}
void webrtc_smoke(EventHandler handler) {
  lk::EventEmitter events(std::move(handler)); lk::webrtc_smoke(events); events.dispatch(); events.rethrow();
}
std::vector<MediaSpec> probe_media(const std::string& input, const std::string& ffmpeg, bool loop, int width, int height, int fps) {
  return lk::probe_media(input, ffmpeg, loop, width, height, fps);
}
}
