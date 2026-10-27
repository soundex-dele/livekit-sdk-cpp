#include "livekit/livekit.hpp"
#include "engines/rtc/room.hpp"
#include "engines/rtc/session.hpp"
#include "engines/rtc/reconnect_policy.hpp"
#include <set>
#include <algorithm>
#include <random>

namespace livekit::sdk {
namespace {
void validate(const RoomOptions& o) {
  detail::validate_reconnect(o.reconnect);
  endpoint(o.url, true);
  detail::signal_endpoint(o.url);
  for (const auto& url : o.regions.trusted_endpoints) detail::trusted_destination(o.url, url, o.regions);
  if (o.regions.max_redirects > 10 || o.regions.discovery_timeout <= std::chrono::milliseconds::zero() ||
      o.regions.discovery_timeout > std::chrono::seconds(30)) throw std::invalid_argument("Invalid region discovery options");
  if (o.token.empty() && !o.token_provider) throw std::invalid_argument("Token or token provider is required");
  if (o.token_timeout <= std::chrono::milliseconds::zero() || o.token_timeout > std::chrono::minutes(5))
    throw std::invalid_argument("Invalid token provider timeout");
  if (o.connect_timeout <= 0 || o.duration < 0) throw std::invalid_argument("Invalid room timeout/duration");
  if (o.signal_protocol != 12 && o.signal_protocol != 15) throw std::invalid_argument("Supported signal protocols are 12 and 15");
  if (o.stats_interval_ms && (o.stats_interval_ms < 200 || o.stats_interval_ms > 60000))
    throw std::invalid_argument("stats_interval_ms must be 0 or 200..60000");
  if (o.relay_only && o.disable_turn) throw std::invalid_argument("relay_only conflicts with disable_turn");
  const bool local_preview = std::any_of(o.media.begin(),o.media.end(),[](const auto& m){return m.video&&m.local_preview;});
  if (o.video_frame_handler && o.video_frame_stream) throw std::invalid_argument("Choose video_frame_handler or video_frame_stream");
  if (!o.subscribe) {
    if (o.audio_frame_stream) throw std::invalid_argument("Remote PCM output requires subscriptions");
    if(!o.record.empty())throw std::invalid_argument("Recording requires subscriptions");
    if(o.video_playout||o.audio_playout)throw std::invalid_argument("Remote playout requires subscriptions");
    if((o.video_frame_handler||o.video_frame_stream)&&!local_preview)throw std::invalid_argument("Frame output requires subscriptions or a local preview source");
    if(!o.subscribe_tracks.empty())throw std::invalid_argument("Track selection requires subscriptions");
  }
  if (!std::set<std::string>{"vp8", "vp9", "h264", "av1"}.contains(o.codec)) throw std::invalid_argument("Unsupported video codec");
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
  s->video_output = std::move(video_output); s->audio_output = std::move(audio_output);
  if (s->video_output) s->video_output->advance();
  if (s->audio_output) s->audio_output->advance();
  s->running = true; ++s->generation; s->stop = std::stop_source{};
  s->connection = ConnectionState::connecting; s->room = {}; s->local = {}; s->remote.clear(); s->subscriptions.clear();
  s->reason = 0; s->failure = {}; s->error.clear(); s->runner = {}; s->command_timeout = o.connect_timeout;
  s->signal_protocol = o.signal_protocol;
  s->recovery = detail::RecoveryState{};
  s->recovery.budget = detail::RecoveryBudget(o.reconnect);
  s->recovery.token = o.token;
  s->recovery.expected_room = o.room;
  for (const auto& media : o.media) {
    const auto id = s->recovery.allocate_id(media.video);
    s->recovery.publications.emplace(id, detail::DesiredPublication{media, {}, false, false});
  }
  if (o.microphone) s->recovery.publications.emplace("microphone", detail::DesiredPublication{{}, {}, true, false});
}
void execute(const std::shared_ptr<detail::RoomState>& s, const RoomOptions& o, EventHandler handler, std::stop_token external) noexcept {
  { std::lock_guard lock(s->mutex); s->runner = std::this_thread::get_id(); }
  std::stop_callback cancel(external, [s] { s->stop.request_stop(); if (s->video_output) s->video_output->close(); if (s->audio_output) s->audio_output->close(); });
  std::exception_ptr failure;
  lk::EventEmitter events([s, handler = std::move(handler)](const Json& event) {
    detail::process_event(s, event); if (handler) handler(event);
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
        if (!o.reconnect.enabled || !recovery.ever_connected || !error.retryable() ||
            (error.domain() != ErrorDomain::signaling && error.domain() != ErrorDomain::transport)) throw;
        recovery.validate_sources();
        const auto attempt = recovery.budget.next_attempt();
        if (s->video_output) s->video_output->advance();
        if (s->audio_output) s->audio_output->advance();
        {
          std::lock_guard lock(s->mutex);
          if (!recovery.rejoining) recovery.local_snapshot = s->local;
          recovery.rejoining = true;
          ++s->generation; // complete rejoin invalidates all old public handles
          s->connection = ConnectionState::reconnecting;
          s->remote.clear(); s->subscriptions.clear(); s->local = {}; s->room = {}; s->reason = 0;
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
  if (s->video_output) s->video_output->close();
  if (s->audio_output) s->audio_output->close();
  for (auto& command : s->take_commands()) command->interrupt(ErrorCode::session_closed, "Room disconnected before command completed");
  int reason;
  { std::lock_guard lock(s->mutex); s->connection = ConnectionState::disconnected; if (!s->reason) s->reason = failure ? 9 : 1; reason = s->reason; }
  try { if (s->callbacks.on_disconnected) s->callbacks.on_disconnected(reason); }
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
Room::~Room() {
  request_stop();
  std::thread worker;
  { std::lock_guard lock(impl_->lifecycle); worker = std::move(impl_->worker); }
  if (worker.joinable()) { if (worker.get_id() == std::this_thread::get_id()) worker.detach(); else worker.join(); }
}
void Room::request_stop() noexcept {
  std::stop_source stop;
  std::shared_ptr<detail::VideoFrameOutput> video;
  std::shared_ptr<detail::AudioFrameOutput> audio;
  { std::lock_guard lock(impl_->state->mutex); stop = impl_->state->stop; video = impl_->state->video_output; audio = impl_->state->audio_output; }
  stop.request_stop();
  if (video) video->close();
  if (audio) audio->close();
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
      if (s->video_output) s->video_output->close(); if (s->audio_output) s->audio_output->close(); throw; }
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
  { std::lock_guard lock(impl_->lifecycle); if (impl_->worker.get_id() == std::this_thread::get_id()) return; worker = std::move(impl_->worker); }
  if (worker.joinable()) worker.join();
}
void Room::wait() {
  auto s = impl_->state;
  { std::unique_lock lock(s->mutex); if (s->runner == std::this_thread::get_id()) throw std::logic_error("Cannot wait on the callback thread"); s->cv.wait(lock, [&] { return !s->running; }); }
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
