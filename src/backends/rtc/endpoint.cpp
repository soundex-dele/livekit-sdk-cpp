#include "backends/rtc/endpoint.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace livekit::sdk::rtc::detail {

void validate_receive_demand(const ReceiveDemand& d, MediaKind kind) {
  if (d.quality != ReceiveQuality::low && d.quality != ReceiveQuality::medium && d.quality != ReceiveQuality::high)
    throw std::invalid_argument("Invalid receive quality");
  if (d.width > 16384 || d.height > 16384 || (d.width == 0) != (d.height == 0) || d.fps > 240)
    throw std::invalid_argument("Invalid receive dimensions or frame rate");
  if (kind == MediaKind::audio && (d.width || d.height || d.fps || d.quality != ReceiveQuality::high))
    throw std::invalid_argument("Audio receive demand cannot select video dimensions or quality");
}

void validate_descriptor(const TrackDescriptor& d, size_t room_budget, bool publication) {
  if (d.kind != MediaKind::audio && d.kind != MediaKind::video)
    throw std::invalid_argument("Invalid RTC media kind");
  if (!publication) validate_receive_demand(d.receive_demand, d.kind);
  if (d.follow_subscriber_demand && (!publication || d.kind != MediaKind::video || d.encodings.size() != 1))
    throw std::invalid_argument("Aggregate subscriber demand requires a single-layer video publication");
  if (publication) switch (d.source) {
    case PublicationSource::unknown: break;
    case PublicationSource::camera: case PublicationSource::screen:
      if (d.kind != MediaKind::video) throw std::invalid_argument("Video source requires a video track");
      break;
    case PublicationSource::microphone: case PublicationSource::screen_audio:
      if (d.kind != MediaKind::audio) throw std::invalid_argument("Audio source requires an audio track");
      break;
    default: throw std::invalid_argument("Invalid RTC publication source");
  }
  switch (d.format.codec) {
    case Codec::opus: case Codec::vp8: case Codec::h264: case Codec::h265: break;
    default: throw std::invalid_argument("Invalid RTC codec");
  }
  if (d.id.empty() || d.id.size() > 256 || !d.priority || d.priority > 16)
    throw std::invalid_argument("Invalid RTC track identity or priority");
  if (!d.queue.max_frames || d.queue.max_frames > 4096 || !d.queue.max_bytes ||
      d.queue.max_bytes > room_budget || d.queue.max_age <= std::chrono::milliseconds::zero() ||
      d.queue.max_age > std::chrono::seconds(5))
    throw std::invalid_argument("Invalid RTC track queue limits");
  if (d.kind == MediaKind::audio) {
    if (d.format.codec != Codec::opus || d.format.clock_rate != 48000 ||
        (d.format.channels != 1 && d.format.channels != 2) || d.encodings.size() != 1)
      throw std::invalid_argument("Encoded audio requires mono/stereo Opus with a 48 kHz RTP clock");
  } else {
    if (d.format.codec == Codec::opus || d.format.clock_rate != 90000 ||
        (publication && (!d.format.width || !d.format.height)) || d.format.width > 16384 || d.format.height > 16384)
      throw std::invalid_argument("Invalid encoded video format");
  }
  if (d.encodings.empty() || d.encodings.size() > 3)
    throw std::invalid_argument("RTC tracks require one to three declared encodings");
  std::set<std::string> ids;
  for (const auto& encoding : d.encodings) {
    if (!ids.insert(encoding.id).second || (d.encodings.size() > 1 && encoding.id.empty()) ||
        encoding.id.size() > 16 || !encoding.max_bitrate_bps)
      throw std::invalid_argument("Invalid RTC encoding descriptor");
  }
}

Endpoint::Endpoint(TrackDescriptor desc, BindingToken binding, std::shared_ptr<QueueBudget> budget)
    : descriptor(std::move(desc)), publication(binding.publication), binding_(std::move(binding)),
      budget_(std::make_shared<QueueBudget>(descriptor.queue.max_bytes, descriptor.queue.max_frames, std::move(budget))) {
  stats_.id = descriptor.id;
  stats_.epoch = binding_.epoch;
  stats_.muted = publication && descriptor.muted;
  stats_.receive_demand = publication ? ReceiveDemand{} : descriptor.receive_demand;
  feedback_.epoch = binding_.epoch;
  for (const auto& encoding : descriptor.encodings) {
    feedback_.encodings[encoding.id] = {0, encoding.active && !stats_.muted, descriptor.kind == MediaKind::video, 0};
    last_timestamp_[encoding.id] = {};
  }
}
Endpoint::~Endpoint() { std::lock_guard lock(mutex_); clear_locked(); }
void Endpoint::set_receive_demand(ReceiveDemand demand) {
  std::lock_guard lock(mutex_); stats_.receive_demand = demand;
}
TrackSnapshot Endpoint::snapshot() const {
  std::lock_guard lock(mutex_);
  auto stats = stats_; stats.outstanding_frames = budget_->frames; stats.outstanding_bytes = budget_->used;
  return stats;
}
SenderFeedback Endpoint::feedback() const { std::lock_guard lock(mutex_); return feedback_; }
BindingToken Endpoint::binding() const { std::lock_guard lock(mutex_); return binding_; }
bool Endpoint::matches(const BindingToken& token) const { std::lock_guard lock(mutex_); return binding_ == token; }

void Endpoint::clear_locked() {
  stats_.dropped += queue_.size();
  queue_.clear();
  stats_.queued_frames = stats_.queued_bytes = 0;
}
void Endpoint::require_keyframe_locked() {
  if (descriptor.kind != MediaKind::video) return;
  for (auto& [id, encoding] : feedback_.encodings) {
    encoding.keyframe_required = true;
    ++encoding.keyframe_requests;
  }
  ++feedback_.revision;
}
void Endpoint::require_keyframe() {
  std::lock_guard lock(mutex_);
  if (stats_.state != TrackState::ready) return;
  clear_locked();
  require_keyframe_locked();
  changed_.notify_all();
}
void Endpoint::expire_locked() {
  // Dropping an encoded frame invalidates the reference chain. Clear queued
  // dependants, not merely the oldest compressed frame, then ask for recovery.
  if (!queue_.empty() && SteadyClock::now() - queue_.front().queued > descriptor.queue.max_age) {
    clear_locked();
    require_keyframe_locked();
  }
}
void Endpoint::rebind(Epoch epoch) {
  std::lock_guard lock(mutex_);
  transport_feedback_ = {};
  clear_locked();
  binding_.epoch = stats_.epoch = feedback_.epoch = epoch;
  stats_.state = TrackState::suspended;
  stats_.server_sid.clear();
  for (auto& [id, timestamp] : last_timestamp_) timestamp.reset();
  for (const auto& encoding : descriptor.encodings)
    feedback_.encodings.at(encoding.id).active = encoding.active && !stats_.muted;
  require_keyframe_locked();
  ++feedback_.revision;
  changed_.notify_all();
}
void Endpoint::ready(std::string sid) {
  std::lock_guard lock(mutex_);
  stats_.server_sid = std::move(sid);
  stats_.state = TrackState::ready;
  ++feedback_.revision;
  changed_.notify_all();
}
void Endpoint::finish(TrackState state, std::string error) {
  std::lock_guard lock(mutex_);
  stats_.state = state;
  stats_.error = std::move(error);
  clear_locked();
  changed_.notify_all();
}
WriteStatus Endpoint::write(EncodedFrame frame) {
  std::lock_guard lock(mutex_);
  if (stats_.state == TrackState::closed || stats_.state == TrackState::failed) return WriteStatus::closed;
  if (frame.epoch != binding_.epoch) return WriteStatus::stale_epoch;
  if (stats_.state != TrackState::ready) return WriteStatus::not_ready;
  if (frame.captured && mute_boundary_ && *frame.captured < *mute_boundary_) return WriteStatus::not_ready;
  auto encoding = feedback_.encodings.find(frame.encoding_id);
  const auto now = SteadyClock::now();
  if (!frame.buffer.size() || frame.config_revision != 1 || encoding == feedback_.encodings.end() ||
      frame.duration < std::chrono::microseconds::zero() || frame.duration > std::chrono::seconds(1) ||
      (descriptor.kind == MediaKind::audio && frame.duration == std::chrono::microseconds::zero()) ||
      (frame.captured && (*frame.captured > now + std::chrono::seconds(1))))
    return WriteStatus::invalid_frame;
  const auto bytes = frame.buffer.bytes();
  if ((descriptor.format.codec == Codec::h264 || descriptor.format.codec == Codec::h265) &&
      (bytes.size() < 5 || bytes[0] || bytes[1] || !(bytes[2] == 1 || (bytes[2] == 0 && bytes[3] == 1))))
    return WriteStatus::invalid_frame;
  auto& last_timestamp = last_timestamp_.at(frame.encoding_id);
  if (last_timestamp && frame.timestamp <= *last_timestamp) return WriteStatus::invalid_frame;
  if (!encoding->second.active) return WriteStatus::not_ready;
  expire_locked();
  if (encoding->second.keyframe_required && !frame.keyframe) return WriteStatus::keyframe_required;
  const auto size = frame.buffer.size();
  if ((frame.captured && now - *frame.captured > descriptor.queue.max_age) ||
      size > descriptor.queue.max_bytes || queue_.size() >= descriptor.queue.max_frames ||
      stats_.queued_bytes > descriptor.queue.max_bytes - size || !budget_->charge(frame.buffer)) {
    ++stats_.dropped;
    if (descriptor.kind == MediaKind::video) { clear_locked(); require_keyframe_locked(); }
    return WriteStatus::backpressure;
  }
  if (frame.keyframe) { encoding->second.keyframe_required = false; ++feedback_.revision; }
  last_timestamp = frame.timestamp;
  // Use admission time to bound queue residence. Media timestamps are never
  // interpreted as wall-clock time; captured is an optional local clock hint.
  const auto admitted = QueueBudget::admitted(frame.buffer);
  queue_.push_back({std::move(frame), ++next_serial_, admitted});
  ++stats_.accepted;
  stats_.queued_frames = queue_.size();
  stats_.queued_bytes += size;
  changed_.notify_all();
  return WriteStatus::accepted;
}
ReadResult Endpoint::read(std::chrono::milliseconds timeout, std::stop_token stop) {
  if (timeout < std::chrono::milliseconds::zero()) throw std::invalid_argument("Negative RTC read timeout");
  std::unique_lock lock(mutex_);
  changed_.wait_for(lock, stop, timeout, [&] { return stats_.state != TrackState::ready || !queue_.empty(); });
  if (stop.stop_requested()) return {ReadStatus::cancelled, {}};
  if (stats_.state == TrackState::closed) return {ReadStatus::closed, {}};
  if (stats_.state == TrackState::failed) return {ReadStatus::failed, {}};
  if (stats_.state != TrackState::ready) return {ReadStatus::suspended, {}};
  expire_locked();
  if (queue_.empty()) return {ReadStatus::timeout, {}};
  auto item = std::move(queue_.front());
  queue_.pop_front();
  const auto bytes = item.frame.buffer.size();
  stats_.queued_bytes -= bytes;
  stats_.queued_frames = queue_.size();
  ++stats_.consumed;
  QueueBudget::release_to_caller(item.frame.buffer);
  return {ReadStatus::frame, std::move(item.frame)};
}
std::optional<Endpoint::Item> Endpoint::front() {
  std::lock_guard lock(mutex_);
  expire_locked();
  if (stats_.state != TrackState::ready || queue_.empty()) return {};
  return queue_.front();
}
void Endpoint::consumed(uint64_t serial, bool dropped) {
  std::lock_guard lock(mutex_);
  if (queue_.empty() || queue_.front().serial != serial) return;
  const auto bytes = queue_.front().frame.buffer.size();
  queue_.pop_front();
  stats_.queued_bytes -= bytes;
  stats_.queued_frames = queue_.size();
  if (dropped) { ++stats_.dropped; clear_locked(); require_keyframe_locked(); }
  else ++stats_.consumed;
}
void Endpoint::update_feedback(const SenderFeedback& feedback) {
  std::lock_guard lock(mutex_);
  if (feedback.epoch != binding_.epoch || stats_.state != TrackState::ready) return;
  if (transport_feedback_.epoch == feedback.epoch && feedback.revision <= transport_feedback_.revision) return;
  for (const auto& [id, update] : feedback.encodings) {
    auto it = feedback_.encodings.find(id);
    if (it == feedback_.encodings.end()) continue;
    auto& current = it->second;
    const auto configured = std::find_if(descriptor.encodings.begin(), descriptor.encodings.end(), [&](const auto& e) { return e.id == id; });
    const bool active = update.active && configured->active && !stats_.muted;
    const auto previous = transport_feedback_.encodings.find(id);
    const bool requested = update.keyframe_required &&
        (previous == transport_feedback_.encodings.end() || !previous->second.keyframe_required ||
         update.keyframe_requests > previous->second.keyframe_requests);
    if (requested || current.active != active) {
      // Clear all queued layers conservatively; no reference-dependent packet
      // may escape after a transport-level recovery request.
      clear_locked();
      require_keyframe_locked();
    }
    current.active = active;
    current.target_bitrate_bps = update.target_bitrate_bps;
  }
  transport_feedback_ = feedback;
  ++feedback_.revision;
}
void Endpoint::set_muted(bool muted) {
  std::lock_guard lock(mutex_);
  if (!publication || stats_.muted == muted || stats_.state == TrackState::closed || stats_.state == TrackState::failed) return;
  stats_.muted = muted; mute_boundary_ = SteadyClock::now(); ++feedback_.media_generation;
  clear_locked(); require_keyframe_locked();
  for (const auto& configured : descriptor.encodings) {
    const auto transport = transport_feedback_.encodings.find(configured.id);
    feedback_.encodings.at(configured.id).active = !muted && configured.active &&
        (transport == transport_feedback_.encodings.end() || transport->second.active);
  }
  ++feedback_.revision; changed_.notify_all();
}

} // namespace livekit::sdk::rtc::detail
