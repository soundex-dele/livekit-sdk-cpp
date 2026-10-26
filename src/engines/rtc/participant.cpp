#include "livekit/participant.hpp"
#include "engines/rtc/session.hpp"
#include <algorithm>

namespace livekit::sdk::detail {
void RoomState::enqueue(uint64_t expected, const std::shared_ptr<Command>& command) {
  std::lock_guard lock(mutex);
  if (!running || stop.stop_requested() || generation != expected || connection != ConnectionState::connected)
    throw std::logic_error("Participant handle is stale or room is not connected");
  if (commands.size() >= 1024) throw std::runtime_error("Room command queue is full");
  command->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(command_timeout);
  commands.push_back(command);
}
std::deque<std::shared_ptr<Command>> RoomState::take_commands() {
  std::lock_guard lock(mutex); std::deque<std::shared_ptr<Command>> result; result.swap(commands); return result;
}
TrackInfo parse_track(const Json& v) {
  TrackInfo t;
  t.sid = v.value("sid", ""); t.name = v.value("name", ""); t.mime_type = v.value("mimeType", "");
  t.kind = static_cast<TrackKind>(v.value("type", 0)); t.source = static_cast<TrackSource>(v.value("source", 0));
  t.muted = v.value("muted", false); t.width = v.value("width", 0); t.height = v.value("height", 0);
  return t;
}
ParticipantInfo parse_participant(const Json& v) {
  ParticipantInfo p;
  p.sid = v.value("sid", ""); p.identity = v.value("identity", ""); p.name = v.value("name", ""); p.metadata = v.value("metadata", "");
  p.state = v.value("state", 0); p.kind = v.value("kind", 0); p.disconnect_reason = v.value("disconnectReason", 0);
  p.attributes = v.value("attributes", std::map<std::string, std::string>{});
  for (const auto& track : v.value("tracks", Json::array())) p.tracks.push_back(parse_track(track));
  return p;
}
void validate_media(const MediaSpec& m) {
  if (m.width < 2 || m.width > 3840 || m.height < 2 || m.height > 2160 || m.width % 2 || m.height % 2 || m.fps < 1 || m.fps > 60)
    throw std::invalid_argument("Invalid media dimensions or frame rate");
  if (!m.demo && m.input.empty() && m.capture == CaptureKind::input) throw std::invalid_argument("Media input is required");
  if (m.capture != CaptureKind::input && (!m.video || m.demo || m.loop || m.device_index < 0))
    throw std::invalid_argument("Invalid device capture specification");
}
}
namespace livekit::sdk {
namespace {
template<class T, class Convert> std::future<T> submit(std::weak_ptr<detail::RoomState> weak, uint64_t generation,
    const std::string& operation, Json args, Convert convert, MediaSpec media = {}, TrackPublishOptions publication = {}) {
  auto promise = std::make_shared<std::promise<T>>(); auto future = promise->get_future();
  try {
    auto state = weak.lock(); if (!state) throw std::logic_error("Room has been destroyed");
    auto command = std::make_shared<detail::Command>();
    command->operation = operation; command->args = std::move(args); command->media = std::move(media); command->publication = std::move(publication);
    command->completion = [promise, convert](const Json& value, std::exception_ptr error) {
      try { if (error) std::rethrow_exception(error);
        if constexpr (std::is_void_v<T>) { convert(value); promise->set_value(); }
        else promise->set_value(convert(value));
      } catch (...) { promise->set_exception(std::current_exception()); }
    };
    state->enqueue(generation, command);
  } catch (...) { promise->set_exception(std::current_exception()); }
  return future;
}
std::future<void> submit_void(std::weak_ptr<detail::RoomState> state, uint64_t generation, const std::string& op, Json args) {
  return submit<void>(state, generation, op, std::move(args), [](const Json&) {});
}
}
TrackPublication::TrackPublication(std::weak_ptr<detail::RoomState> s, uint64_t g, std::string owner, TrackInfo snapshot, bool local)
    : state_(std::move(s)), generation_(g), owner_(std::move(owner)), snapshot_(std::move(snapshot)), local_(local) {}
TrackInfo TrackPublication::info() const {
  auto state = state_.lock(); if (!state) return snapshot_;
  std::lock_guard lock(state->mutex);
  if (state->generation != generation_) return snapshot_;
  const ParticipantInfo* participant = nullptr;
  if (local_) participant = &state->local;
  else { auto it = state->remote.find(owner_); if (it != state->remote.end()) participant = &it->second; }
  if (participant) for (const auto& t : participant->tracks) if (t.sid == snapshot_.sid) return t;
  return snapshot_;
}
std::future<void> TrackPublication::set_muted(bool muted) const {
  if (!local_) throw std::logic_error("Only local publications can be muted");
  return submit_void(state_, generation_, "mute", {{"sid", sid()}, {"muted", muted}});
}
std::future<void> TrackPublication::set_subscribed(bool subscribed) const {
  if (local_) throw std::logic_error("Cannot subscribe to a local publication");
  return submit_void(state_, generation_, "subscribe", {{"sid", sid()}, {"subscribe", subscribed}});
}
std::future<void> TrackPublication::set_enabled(bool enabled) const {
  if (local_) throw std::logic_error("Stream settings apply to remote publications");
  return submit_void(state_, generation_, "settings", {{"sid", sid()}, {"disabled", !enabled}});
}
std::future<void> TrackPublication::set_video_dimensions(int width, int height) const {
  if (local_ || width <= 0 || height <= 0) throw std::invalid_argument("Positive remote video dimensions required");
  return submit_void(state_, generation_, "settings", {{"sid", sid()}, {"width", width}, {"height", height}});
}
Participant::Participant(std::weak_ptr<detail::RoomState> s, uint64_t g, ParticipantInfo snapshot, bool local)
    : state_(std::move(s)), generation_(g), snapshot_(std::move(snapshot)), local_(local) {}
ParticipantInfo Participant::info() const {
  auto state = state_.lock(); if (!state) return snapshot_;
  std::lock_guard lock(state->mutex);
  if (state->generation != generation_) return snapshot_;
  if (local_) return state->local;
  auto found = state->remote.find(snapshot_.identity); return found == state->remote.end() || found->second.sid != snapshot_.sid ? snapshot_ : found->second;
}
std::vector<TrackPublication> Participant::track_publications() const {
  std::vector<TrackPublication> result;
  for (auto& t : info().tracks) result.emplace_back(state_, generation_, identity(), t, local_);
  return result;
}
std::future<TrackPublication> LocalParticipant::publish_track(const MediaSpec& media, const TrackPublishOptions& options) const {
  detail::validate_media(media);
  return submit<TrackPublication>(state_, generation_, "publish", {},
      [weak = state_, generation = generation_, owner = identity()](const Json& result) {
        return TrackPublication(weak, generation, owner, detail::parse_track(result), true);
      }, media, options);
}
std::future<void> LocalParticipant::unpublish_track(const std::string& sid) const { return submit_void(state_, generation_, "unpublish", {{"sid", sid}}); }
std::future<void> LocalParticipant::publish_data(const std::vector<uint8_t>& payload, const DataPublishOptions& options) const {
  // Leave room for DataPacket protobuf headers, identities and topic below the SCTP limit.
  if (payload.size() > (options.reliable ? 15000 : 1300)) throw std::invalid_argument("Data payload exceeds reliable/lossy packet limit");
  std::string bytes(payload.begin(), payload.end());
  return submit_void(state_, generation_, "data", {{"payload", base64url(bytes)}, {"reliable", options.reliable}, {"topic", options.topic}, {"destinations", options.destination_identities}});
}
std::future<void> LocalParticipant::set_name(const std::string& value) const { return submit_void(state_, generation_, "metadata", {{"name", value}}); }
std::future<void> LocalParticipant::set_metadata(const std::string& value) const { return submit_void(state_, generation_, "metadata", {{"metadata", value}}); }
std::future<void> LocalParticipant::set_attributes(const std::map<std::string, std::string>& value) const { return submit_void(state_, generation_, "metadata", {{"attributes", value}}); }
}
