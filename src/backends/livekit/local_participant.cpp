#include "livekit/client/participant.hpp"
#include "backends/livekit/participant_state.hpp"
#include "support/logger.h"

namespace livekit::sdk::client::detail {
Result<LocalTrackPublication> ParticipantData::publish(std::shared_ptr<TrackData> track,
                                                       TrackPublishOptions options,
                                                       RtcSession& session, const Operation& op) {
  auto owner = engine.lock();
  if (!track || !owner) {
    return Result<LocalTrackPublication>::failure(make_failure(
        ErrorDomain::lifecycle, ErrorCode::stale_handle, "Invalid track or participant"));
  }
  TrackConfig config;
  {
    std::lock_guard lock(track->mutex);
    if (track->claimed) {
      return Result<LocalTrackPublication>::failure(make_failure(
          ErrorDomain::media, ErrorCode::busy, "Track already belongs to a publication"));
    }
    config = track->config;
    config.source = options.source;
    config.stream_group = options.stream_group;
    config.encodings = options.encodings;
    config.follow_subscriber_demand = options.follow_subscriber_demand;
    if (auto result = validate_track(config); !result) {
      return Result<LocalTrackPublication>::failure(result.failure_record());
    }
    track->claimed = true;
    track->config = config;
    track->engine = engine;
    track->budget =
        std::make_shared<QueueBudget>(config.queue.max_bytes, config.queue.max_frames, budget);
  }

  struct Claim {
    std::shared_ptr<TrackData> track;
    bool committed = false;

    ~Claim() {
      if (!committed) {
        track->transition(TrackState::pending);
        std::lock_guard lock(track->mutex);
        track->claimed = false;
      }
    }
  } claim{track};

  {
    std::lock_guard lock(mutex);
    if (publications.size() >= owner->options().max_tracks) {
      return Result<LocalTrackPublication>::failure(make_failure(
          ErrorDomain::media, ErrorCode::resource_exhausted, "Publication limit reached"));
    }
  }
  // Rust LocalParticipant: AddTrack acknowledgement, then native sender, then publication.
  auto added = session.add_track(config, op);
  if (!added) {
    return Result<LocalTrackPublication>::failure(added.failure_record());
  }
  auto result = session.create_sender(added->sid, track, op);
  if (!result) {
    auto cleanup = session.remove_track(added->sid, op);
    if (!cleanup) {
      LOG_ERROR("LiveKitClient", "publish rollback: " << cleanup.error().message);
    }
    return Result<LocalTrackPublication>::failure(result.failure_record());
  }
  auto publication = std::make_shared<PublicationData>();
  publication->engine = engine;
  publication->track = track;
  publication->notify = notify;
  publication->options = std::move(options);
  auto& p = publication->info;
  p.track_id = config.id;
  p.sid = added->sid;
  p.name = config.name;
  p.kind = config.kind;
  p.source = config.source;
  p.local = true;
  p.state = TrackState::ready;
  p.muted = config.muted;
  {
    std::lock_guard lock(mutex);
    p.participant_identity = info.identity;
    publications[config.id] = publication;
    info.publications[p.sid] = *added;
  }
  track->transition(TrackState::ready, owner->epoch(), p.sid);
  claim.committed = true;
  publication->emit(RoomEventKind::track_published);
  return Result<LocalTrackPublication>::success(LocalTrackPublication(publication));
}

Result<void> ParticipantData::unpublish(const TrackSid& sid, RtcSession& session,
                                        const Operation& op) {
  std::shared_ptr<PublicationData> publication;
  {
    std::lock_guard lock(mutex);
    for (const auto& [id, p] : publications) {
      std::lock_guard publication_lock(p->mutex);
      if (p->info.sid == sid) {
        publication = p;
        break;
      }
    }
  }
  if (!publication) {
    return Result<void>::failure(
        make_failure(ErrorDomain::media, ErrorCode::stale_handle, "Unknown publication SID"));
  }
  auto result = session.remove_track(sid, op);
  publication->track->transition(TrackState::closed);
  {
    std::lock_guard lock(publication->track->mutex);
    publication->track->claimed = false;
  }
  {
    std::lock_guard lock(publication->mutex);
    publication->info.state = TrackState::closed;
  }
  {
    std::lock_guard lock(mutex);
    publications.erase(publication->info.track_id);
    info.publications.erase(sid);
  }
  publication->emit(RoomEventKind::track_unpublished);
  return result;
}

Result<void> ParticipantData::restore(RtcSession& session, const Operation& op) {
  std::vector<std::shared_ptr<PublicationData>> list;
  {
    std::lock_guard lock(mutex);
    for (auto& [id, p] : publications) {
      list.push_back(p);
    }
  }
  for (auto& p : list) {
    TrackConfig config;
    {
      std::lock_guard lock(p->track->mutex);
      config = p->track->config;
    }
    auto added = session.add_track(config, op);
    if (!added) {
      return Result<void>::failure(added.failure_record());
    }
    if (auto result = session.create_sender(added->sid, p->track, op); !result) {
      return result;
    }
    TrackSid old;
    {
      std::lock_guard lock(p->mutex);
      old = p->info.sid;
      p->info.sid = added->sid;
      p->info.state = TrackState::ready;
    }
    {
      std::lock_guard lock(mutex);
      info.publications.erase(old);
      info.publications[added->sid] = *added;
    }
    p->track->transition(TrackState::ready, engine.lock()->epoch(), added->sid);
    p->emit(RoomEventKind::track_republished, {}, old);
  }
  return Result<void>::success();
}

void ParticipantData::on_event(const SessionEvent& event) {
  std::shared_ptr<PublicationData> publication;
  {
    std::lock_guard lock(mutex);
    for (const auto& [id, p] : publications) {
      std::lock_guard publication_lock(p->mutex);
      if (p->info.sid == event.sid) {
        publication = p;
        break;
      }
    }
  }
  if (!publication) {
    return;
  }
  if (event.kind == SessionEvent::Kind::mute) {
    {
      std::lock_guard lock(publication->track->mutex);
      auto& track = *publication->track;
      track.config.muted = track.info.muted = event.muted;
      track.needs_keyframe = true;
      if (track.mute_sender) {
        track.mute_sender(event.muted);
      }
    }
    {
      std::lock_guard lock(publication->mutex);
      publication->info.muted = event.muted;
    }
    publication->emit(RoomEventKind::track_muted);
  } else if (event.kind == SessionEvent::Kind::feedback) {
    RoomEvent value;
    value.kind = RoomEventKind::sender_feedback;
    value.feedback = event.feedback;
    {
      std::lock_guard lock(publication->mutex);
      value.publication = publication->info;
    }
    value.epoch = event.feedback.epoch;
    if (notify) {
      notify(std::move(value));
    }
  }
}

void ParticipantData::suspend() {
  std::lock_guard lock(mutex);
  for (auto& [id, p] : publications) {
    std::lock_guard publication_lock(p->mutex);
    p->info.state = TrackState::suspended;
    if (p->track) {
      p->track->transition(TrackState::suspended);
    }
  }
}

void ParticipantData::close() {
  std::lock_guard lock(mutex);
  for (auto& [id, p] : publications) {
    std::lock_guard publication_lock(p->mutex);
    p->info.state = TrackState::closed;
    p->info.subscribed = false;
    if (p->track) {
      p->track->transition(TrackState::closed);
      std::lock_guard track_lock(p->track->mutex);
      p->track->claimed = false;
    }
  }
  // A failed room may be explicitly closed later. It must not touch a track
  // already claimed by another room after failure released this publication.
  publications.clear();
  info.publications.clear();
}
}  // namespace livekit::sdk::client::detail

namespace livekit::sdk::client {
ParticipantSnapshot LocalParticipant::snapshot() const {
  if (!data_) {
    return {};
  }
  std::lock_guard lock(data_->mutex);
  return data_->info;
}

ParticipantSnapshot RemoteParticipant::snapshot() const {
  if (!data_) {
    return {};
  }
  std::lock_guard lock(data_->mutex);
  return data_->info;
}

std::vector<LocalTrackPublication> LocalParticipant::track_publications() const {
  std::vector<LocalTrackPublication> list;
  if (data_) {
    std::lock_guard lock(data_->mutex);
    for (auto& [id, p] : data_->publications) {
      list.push_back(LocalTrackPublication(p));
    }
  }
  return list;
}

std::vector<RemoteTrackPublication> RemoteParticipant::track_publications() const {
  std::vector<RemoteTrackPublication> list;
  if (data_) {
    std::lock_guard lock(data_->mutex);
    for (auto& [id, p] : data_->publications) {
      list.push_back(RemoteTrackPublication(p));
    }
  }
  return list;
}

Result<LocalTrackPublication> LocalParticipant::publish_track(LocalTrack track,
                                                              TrackPublishOptions options) {
  auto engine = data_ ? data_->engine.lock() : nullptr;
  if (!engine) {
    return Result<LocalTrackPublication>::failure(
        make_failure(ErrorDomain::lifecycle, ErrorCode::stale_handle, "Detached participant"));
  }
  return engine->call<LocalTrackPublication>(
      [data = data_, track = track.data_, options](detail::RtcSession& s,
                                                   const detail::Operation& op) {
        return data->publish(track, options, s, op);
      });
}

void LocalParticipant::unpublish_track(TrackSid sid) {
  auto engine = data_ ? data_->engine.lock() : nullptr;
  if (!engine) {
    LOG_ERROR("LiveKitClient", "unpublish_track: detached participant");
    return;
  }
  auto result =
      engine->perform([data = data_, sid](detail::RtcSession& s, const detail::Operation& op) {
        return data->unpublish(sid, s, op);
      });
  if (!result) {
    LOG_ERROR("LiveKitClient", "unpublish_track: " << result.error().message);
  }
}

void LocalParticipant::publish_data(DataPacket packet) {
  auto engine = data_ ? data_->engine.lock() : nullptr;
  if (!engine) {
    LOG_ERROR("LiveKitClient", "publish_data: detached participant");
    return;
  }
  auto result = engine->perform(
      [packet = std::move(packet)](detail::RtcSession& s, const detail::Operation& op) {
        return s.publish_data(packet, op);
      });
  if (!result) {
    LOG_ERROR("LiveKitClient", "publish_data: " << result.error().message);
  }
}

void LocalParticipant::update(ParticipantUpdate update) {
  auto engine = data_ ? data_->engine.lock() : nullptr;
  if (!engine) {
    LOG_ERROR("LiveKitClient", "update: detached participant");
    return;
  }
  auto result = engine->perform(
      [update = std::move(update)](detail::RtcSession& s, const detail::Operation& op) {
        return s.update(update, op);
      });
  if (!result) {
    LOG_ERROR("LiveKitClient", "update: " << result.error().message);
  }
}
}  // namespace livekit::sdk::client
