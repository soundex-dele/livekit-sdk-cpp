#include "livekit/participant.hpp"
#include "livekit/video_source.hpp"
#include "engines/rtc/session.hpp"
#include "engines/rtc/data_messages.hpp"
#include "engines/rtc/subscription_permissions.hpp"
#include <algorithm>

namespace livekit::sdk::detail {
void RoomState::enqueue(uint64_t expected, const std::shared_ptr<Command>& command) {
  if (command->options.timeout < std::chrono::milliseconds::zero() || command->options.timeout > std::chrono::minutes(5))
    throw std::invalid_argument("Invalid operation timeout");
  if (command->options.wait_for_ack && command->operation != "metadata" && command->operation != "publish")
    throw SDKError(ErrorDomain::protocol, ErrorCode::unsupported, "This operation has no server acknowledgement contract");
  std::lock_guard lock(mutex);
  if (command->done()) return;
  if (command->options.wait_for_ack && command->operation == "metadata" && signal_protocol < 15)
    throw SDKError(ErrorDomain::protocol, ErrorCode::unsupported, "Metadata acknowledgement requires RoomOptions.signal_protocol = 15");
  if (!running || stop.stop_requested() || generation != expected || connection != ConnectionState::connected)
    throw std::logic_error("Participant handle is stale or room is not connected");
  if (commands.size() >= 1024) throw std::runtime_error("Room command queue is full");
  command->deadline = std::chrono::steady_clock::now() + (command->options.timeout.count() ?
      command->options.timeout : std::min(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(command_timeout)), std::chrono::milliseconds(300000)));
  if (!deadlines) deadlines = std::make_unique<OperationDeadlines>();
  deadlines->add(command);
  commands.push_back(command);
}
std::deque<std::shared_ptr<Command>> RoomState::take_commands() {
  std::lock_guard lock(mutex); std::deque<std::shared_ptr<Command>> result; result.swap(commands); return result;
}
TrackInfo parse_track(const Json& v) {
  TrackInfo t;
  t.sid = v.value("sid", ""); t.name = v.value("name", ""); t.mime_type = v.value("mimeType", "");
  t.client_id = v.value("clientId", "");
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
  if (!m.demo && !m.video_source && !m.audio_source && m.input.empty() && m.capture == CaptureKind::input) throw std::invalid_argument("Media input is required");
  if (m.audio_source && (m.video || m.video_source || m.demo || m.loop || !m.input.empty() ||
      m.capture != CaptureKind::input || m.local_preview)) throw std::invalid_argument("Invalid custom PCM media specification");
  if (m.video_source) {
    const auto format = m.video_source->options();
    if (!m.video || m.demo || m.loop || !m.input.empty() || m.capture != CaptureKind::input ||
        m.width != format.width || m.height != format.height)
      throw std::invalid_argument("Invalid custom I420 media specification");
  }
  if (m.capture != CaptureKind::input && (!m.video || m.demo || m.loop || m.device_index < 0))
    throw std::invalid_argument("Invalid device capture specification");
}
}
namespace livekit::sdk {
namespace {
template<class T, class Convert> std::future<T> submit(std::weak_ptr<detail::RoomState> weak, uint64_t generation,
    const std::string& operation, Json args, Convert convert, MediaSpec media = {}, TrackPublishOptions publication = {}, OperationOptions options = {}) {
  auto promise = std::make_shared<std::promise<T>>(); auto future = promise->get_future();
  std::shared_ptr<detail::Command> command;
  try {
    auto state = weak.lock(); if (!state) throw std::logic_error("Room has been destroyed");
    command = std::make_shared<detail::Command>();
    command->operation = operation; command->args = std::move(args); command->media = std::move(media); command->publication = std::move(publication);
    command->options = std::move(options);
    command->completion = [promise, convert](const Json& value, std::exception_ptr error) {
      try { if (error) std::rethrow_exception(error);
        if constexpr (std::is_void_v<T>) { convert(value); promise->set_value(); }
        else promise->set_value(convert(value));
      } catch (...) { promise->set_exception(std::current_exception()); }
    };
    command->arm_cancellation();
    state->enqueue(generation, command);
  } catch (...) {
    if (command && command->completion) command->reject(std::current_exception());
    else promise->set_exception(std::current_exception());
  }
  return future;
}
std::future<void> submit_void(std::weak_ptr<detail::RoomState> state, uint64_t generation, const std::string& op, Json args, OperationOptions options = {}) {
  return submit<void>(state, generation, op, std::move(args), [](const Json&) {}, {}, {}, std::move(options));
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
std::future<void> TrackPublication::set_muted(bool muted, const OperationOptions& operation) const {
  if (!local_) throw std::logic_error("Only local publications can be muted");
  return submit_void(state_, generation_, "mute", {{"sid", sid()}, {"muted", muted}}, operation);
}
std::future<void> TrackPublication::set_subscribed(bool subscribed, const OperationOptions& operation) const {
  if (local_) throw std::logic_error("Cannot subscribe to a local publication");
  return submit_void(state_, generation_, "subscribe", {{"sid", sid()}, {"subscribe", subscribed}}, operation);
}
std::future<void> TrackPublication::set_enabled(bool enabled, const OperationOptions& operation) const {
  if (local_) throw std::logic_error("Stream settings apply to remote publications");
  return submit_void(state_, generation_, "settings", {{"sid", sid()}, {"disabled", !enabled}}, operation);
}
std::future<void> TrackPublication::set_video_dimensions(int width, int height, const OperationOptions& operation) const {
  if (local_ || width <= 0 || height <= 0) throw std::invalid_argument("Positive remote video dimensions required");
  return submit_void(state_, generation_, "settings", {{"sid", sid()}, {"width", width}, {"height", height}}, operation);
}
std::future<void> TrackPublication::set_video_quality(VideoQuality quality, const OperationOptions& operation) const {
  const auto value=static_cast<int>(quality);
  if(local_ || snapshot_.kind!=TrackKind::video || value<0 || value>2)throw std::invalid_argument("Valid remote video quality required");
  return submit_void(state_,generation_,"settings",{{"sid",sid()},{"quality",value},{"width",0},{"height",0}},operation);
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
std::future<TrackPublication> LocalParticipant::publish_track(const MediaSpec& media, const TrackPublishOptions& options, const OperationOptions& operation) const {
  detail::validate_media(media);
  return submit<TrackPublication>(state_, generation_, "publish", {},
      [weak = state_, generation = generation_, owner = identity()](const Json& result) {
        return TrackPublication(weak, generation, owner, detail::parse_track(result), true);
      }, media, options, operation);
}
std::future<void> LocalParticipant::unpublish_track(const std::string& sid, const OperationOptions& operation) const { return submit_void(state_, generation_, "unpublish", {{"sid", sid}}, operation); }
std::future<void> LocalParticipant::publish_data(const std::vector<uint8_t>& payload, const DataPublishOptions& options, const OperationOptions& operation) const {
  // Leave room for DataPacket protobuf headers, identities and topic below the SCTP limit.
  if (payload.size() > (options.reliable ? 15000 : 1300)) throw std::invalid_argument("Data payload exceeds reliable/lossy packet limit");
  std::string bytes(payload.begin(), payload.end());
  return submit_void(state_, generation_, "data", {{"payload", base64url(bytes)}, {"reliable", options.reliable}, {"topic", options.topic}, {"destinations", options.destination_identities}}, operation);
}
std::future<void> LocalParticipant::set_name(const std::string& value, const OperationOptions& operation) const { return submit_void(state_, generation_, "metadata", {{"name", value}}, operation); }
std::future<void> LocalParticipant::publish_dtmf(const DtmfDigit& digit, const OperationOptions& operation) const {
  return submit_void(state_, generation_, "packet", {{"payload",base64url(detail::encode_dtmf(digit).SerializeAsString())},{"reliable",true}}, operation);
}
std::future<void> LocalParticipant::set_subscription_permissions(const SubscriptionPermissions& permissions,const OperationOptions& operation) const {
  return submit_void(state_,generation_,"permissions",detail::permissions_json(permissions),operation);
}
std::future<std::string> LocalParticipant::perform_rpc(const RpcCallOptions& options) const {
  auto state=state_.lock();if(!state)throw std::logic_error("Room has been destroyed");
  std::lock_guard lock(state->mutex);
  if(!state->running||state->stop.stop_requested()||state->generation!=generation_||state->connection!=ConnectionState::connected)
    throw std::logic_error("Participant handle is stale or room is not connected");
  if(!state->remote.contains(options.destination_identity)){
    std::promise<std::string> promise;auto result=promise.get_future();
    promise.set_exception(std::make_exception_ptr(RpcError(RpcErrorCode::recipient_not_found,"RPC recipient not found")));return result;
  }
  return state->rpc_session()->call(options);
}
std::future<void> LocalParticipant::publish_transcription(const Transcription& transcription, const OperationOptions& operation) const {
  return submit_void(state_, generation_, "packet", {{"payload",base64url(detail::encode_transcription(transcription).SerializeAsString())},{"reliable",true}}, operation);
}
std::future<void> LocalParticipant::set_metadata(const std::string& value, const OperationOptions& operation) const { return submit_void(state_, generation_, "metadata", {{"metadata", value}}, operation); }
std::future<void> LocalParticipant::set_attributes(const std::map<std::string, std::string>& value, const OperationOptions& operation) const { return submit_void(state_, generation_, "metadata", {{"attributes", value}}, operation); }
}
