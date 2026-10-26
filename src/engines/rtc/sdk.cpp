#include "livekit/livekit.hpp"
#include "engines/rtc/room.hpp"
#include "engines/rtc/session.hpp"
#include <set>

namespace livekit::sdk {
namespace {
void validate(const RoomOptions& o) {
  endpoint(o.url, true);
  if (o.token.empty()) throw std::invalid_argument("Token is required");
  if (o.connect_timeout <= 0 || o.duration < 0) throw std::invalid_argument("Invalid room timeout/duration");
  if (o.stats_interval_ms && (o.stats_interval_ms < 200 || o.stats_interval_ms > 60000))
    throw std::invalid_argument("stats_interval_ms must be 0 or 200..60000");
  if (o.relay_only && o.disable_turn) throw std::invalid_argument("relay_only conflicts with disable_turn");
  if (!o.subscribe && (!o.record.empty() || o.video_playout || o.audio_playout || o.video_frame_handler || !o.subscribe_tracks.empty()))
    throw std::invalid_argument("Recording/playout/track selection requires subscriptions");
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
  s->running = true; ++s->generation; s->stop = std::stop_source{};
  s->connection = ConnectionState::connecting; s->room = {}; s->local = {}; s->remote.clear(); s->subscriptions.clear();
  s->reason = 0; s->failure = {}; s->error.clear(); s->runner = {}; s->command_timeout = o.connect_timeout;
}
void execute(const std::shared_ptr<detail::RoomState>& s, const RoomOptions& o, EventHandler handler, std::stop_token external) noexcept {
  { std::lock_guard lock(s->mutex); s->runner = std::this_thread::get_id(); }
  std::stop_callback cancel(external, [s] { s->stop.request_stop(); });
  std::exception_ptr failure;
  lk::EventEmitter events([s, handler = std::move(handler)](const Json& event) {
    detail::process_event(s, event); if (handler) handler(event);
  });
  try { lk::join_room(o, s->stop.get_token(), events, s); events.dispatch(); events.rethrow(); }
  catch (...) { failure = std::current_exception(); events.dispatch(); }
  auto closed = std::make_exception_ptr(std::runtime_error("Room disconnected before command completed"));
  // Stop enqueueing before draining commands, including commands from callbacks.
  s->stop.request_stop();
  for (auto& command : s->take_commands()) command->reject(closed);
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
  { std::lock_guard lock(impl_->state->mutex); stop = impl_->state->stop; }
  stop.request_stop();
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
    catch (...) { std::lock_guard state_lock(s->mutex); s->running = false; s->connection = ConnectionState::disconnected; throw; }
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
