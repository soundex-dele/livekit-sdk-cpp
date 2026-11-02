#include "backends/livekit/room_session.hpp"
#include "support/logger.h"
#include <set>
#include <algorithm>

namespace livekit::sdk::client::detail {
RoomSession::RoomSession(SessionFactory factory)
    : engine_(std::make_shared<RtcEngine>(std::move(factory))) {
  engine_->state_changed = [this](ConnectionState state, Failure error) { on_state(state, error); };
  engine_->events = [this](SessionEvent e) { on_event(std::move(e)); };
  engine_->joined = [this](const JoinState& join, bool rejoin, RtcSession& s, const Operation& op) {
    return on_join(join, rejoin, s, op);
  };
  notifier_ = std::thread([this] { run_notifications(); });
}

RoomSession::~RoomSession() {
  observer(nullptr);
  close();
  {
    std::lock_guard lock(notification_mutex_);
    notifications_closed_ = true;
  }
  notification_changed_.notify_all();
  notifier_.join();
}

void RoomSession::observer(RoomObserver* observer) {
  if (sdk_thread) {
    LOG_ERROR("LiveKitClient", "set_observer on SDK thread is not allowed");
    return;
  }
  std::lock_guard serial(observer_change_);
  std::unique_lock lock(notification_mutex_);
  observer_ = nullptr;
  notifications_.clear();
  notification_bytes_ = 0;
  notification_changed_.wait(lock, [&] { return !notifying_; });
  observer_ = observer;
}

bool RoomSession::connect(ConnectionOptions options) {
  if (sdk_thread) {
    LOG_ERROR("LiveKitClient", "connect on SDK thread is not allowed");
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    if (state_ != ConnectionState::created) {
      LOG_ERROR("LiveKitClient", "Room is already used");
      return false;
    }
    if (options.url.empty() || options.token.empty() || options.connect_timeout <= 0 ||
        options.operation_timeout.count() <= 0 || !options.max_tracks || !options.max_events ||
        !options.max_queued_bytes || options.reconnect.initial_delay.count() < 0 ||
        options.reconnect.max_delay < options.reconnect.initial_delay ||
        options.reconnect.timeout.count() <= 0) {
      LOG_ERROR("LiveKitClient", "Invalid connection options");
      return false;
    }
    state_ = ConnectionState::connecting;
    options_ = options;
    max_events_ = options.max_events;
    budget_ = std::make_shared<QueueBudget>(options.max_queued_bytes);
  }
  auto result = engine_->connect(std::move(options));
  if (!result) {
    LOG_ERROR("LiveKitClient", "connect failed: " << result.error().message);
  }
  return bool(result);
}

void RoomSession::close() {
  engine_->close();
}

ConnectionState RoomSession::state() const {
  std::lock_guard lock(mutex_);
  return state_;
}

RoomInfo RoomSession::info() const {
  std::lock_guard lock(mutex_);
  return info_;
}

LocalParticipant RoomSession::local() const {
  std::lock_guard lock(mutex_);
  return LocalParticipant(local_);
}

std::vector<RemoteParticipant> RoomSession::remotes() const {
  std::lock_guard lock(mutex_);
  std::vector<RemoteParticipant> list;
  for (auto& [identity, p] : remote_) {
    list.push_back(RemoteParticipant(p));
  }
  return list;
}

Result<void> RoomSession::on_join(const JoinState& join, bool rejoin, RtcSession& session,
                                  const Operation& op) {
  {
    std::lock_guard lock(mutex_);
    info_ = join.room;
    for (auto& [id, p] : remote_) {
      if (rejoin) {
        std::lock_guard participant_lock(p->mutex);
        for (auto& [track_id, publication] : p->publications) {
          std::lock_guard publication_lock(publication->mutex);
          const auto& i = publication->info;
          auto& intents = subscription_intents_[{id, i.name, i.kind, i.source}];
          auto existing = std::find_if(intents.begin(), intents.end(),
                                       [&](const auto& value) { return value.sid == i.sid; });
          SubscriptionIntent value{i.sid, i.desired, publication->receive};
          if (existing != intents.end()) {
            *existing = value;
          } else {
            intents.push_back(value);
          }
        }
      }
      p->close();
    }
    remote_.clear();
    if (!local_) {
      local_ = std::make_shared<ParticipantData>();
      local_->engine = engine_;
      local_->budget = budget_;
      local_->notify = [weak = weak_from_this()](RoomEvent e) {
        if (auto self = weak.lock()) {
          self->notify(std::move(e));
        }
      };
    }
    std::lock_guard local_lock(local_->mutex);
    local_->info = join.local;
  }
  for (const auto& p : join.participants) {
    participant(p);
  }
  return rejoin ? local_->restore(session, op) : Result<void>::success();
}

void RoomSession::on_state(ConnectionState state, Failure error) {
  {
    std::lock_guard lock(mutex_);
    state_ = state;
    if (state == ConnectionState::reconnecting) {
      if (local_) {
        local_->suspend();
      }
      for (auto& [id, p] : remote_) {
        p->suspend();
      }
    }
    if (state == ConnectionState::closing || state == ConnectionState::failed) {
      if (local_) {
        local_->close();
      }
      for (auto& [id, p] : remote_) {
        p->close();
      }
    }
  }
  RoomEvent event;
  event.kind = RoomEventKind::connection_changed;
  event.connection = state;
  event.epoch = engine_->epoch();
  event.failure = std::move(error);
  notify(std::move(event));
}

void RoomSession::participant(ParticipantSnapshot snapshot) {
  std::shared_ptr<ParticipantData> data;
  bool added = false;
  {
    std::lock_guard lock(mutex_);
    if (local_) {
      std::lock_guard local_lock(local_->mutex);
      if (local_->info.identity == snapshot.identity) {
        local_->info = std::move(snapshot);
        return;
      }
    }
    auto found = remote_.find(snapshot.identity);
    if (snapshot.state == 3) {
      if (found == remote_.end()) {
        return;
      }
      data = found->second;
      remote_.erase(found);
    } else {
      if (found == remote_.end()) {
        if (remote_.size() >= options_.max_events) {
          LOG_ERROR("LiveKitClient", "Participant limit reached");
          return;
        }
        data = std::make_shared<ParticipantData>();
        data->engine = engine_;
        data->budget = budget_;
        data->notify = [weak = weak_from_this()](RoomEvent e) {
          if (auto self = weak.lock()) {
            self->notify(std::move(e));
          }
        };
        remote_[snapshot.identity] = data;
        added = true;
      } else {
        data = found->second;
      }
    }
  }
  if (snapshot.state == 3) {
    data->close();
  }
  std::vector<std::shared_ptr<PublicationData>> discovered, removed;
  {
    std::lock_guard lock(data->mutex);
    data->info = snapshot;
    for (auto it = data->publications.begin(); it != data->publications.end();) {
      if (snapshot.state == 3 || !snapshot.publications.count(it->first)) {
        removed.push_back(it->second);
        it = data->publications.erase(it);
      } else {
        ++it;
      }
    }
    if (snapshot.state != 3) {
      for (const auto& [sid, wire] : snapshot.publications) {
        if (wire.kind == PublicationKind::data) {
          continue;
        }
        auto found = data->publications.find(sid);
        if (found != data->publications.end()) {
          std::lock_guard lock(found->second->mutex);
          found->second->info.muted = wire.muted;
          continue;
        }
        if (data->publications.size() >= options_.max_tracks) {
          break;
        }
        auto p = std::make_shared<PublicationData>();
        p->engine = engine_;
        p->notify = data->notify;
        p->info.track_id = sid;
        p->info.sid = sid;
        p->info.name = wire.name;
        p->info.kind = wire.kind == PublicationKind::audio ? MediaKind::audio : MediaKind::video;
        p->info.source = wire.source;
        p->info.muted = wire.muted;
        p->info.participant_identity = snapshot.identity;
        p->info.desired = options_.auto_subscribe;
        auto saved =
            subscription_intents_.find({snapshot.identity, wire.name, p->info.kind, wire.source});
        if (saved != subscription_intents_.end()) {
          auto match = std::find_if(saved->second.begin(), saved->second.end(),
                                    [&](const auto& value) { return value.sid == sid; });
          if (match == saved->second.end() && saved->second.size() == 1) {
            match = saved->second.begin();
          }
          if (match != saved->second.end()) {
            p->info.desired = match->desired;
            p->receive = match->receive;
            saved->second.erase(match);
            if (saved->second.empty()) {
              subscription_intents_.erase(saved);
            }
          }
        }
        p->track = std::make_shared<TrackData>();
        p->track->engine = engine_;
        p->track->config.id = sid;
        p->track->config.kind = p->info.kind;
        p->track->budget = std::make_shared<QueueBudget>(
            p->track->config.queue.max_bytes, p->track->config.queue.max_frames, budget_);
        data->publications[sid] = p;
        discovered.push_back(p);
      }
    }
  }
  RoomEvent event;
  event.kind = snapshot.state == 3 ? RoomEventKind::participant_disconnected
               : added             ? RoomEventKind::participant_connected
                                   : RoomEventKind::participant_changed;
  event.participant_identity = snapshot.identity;
  event.participant = std::move(snapshot);
  notify(std::move(event));
  for (auto& p : removed) {
    {
      std::lock_guard lock(p->mutex);
      p->info.state = TrackState::closed;
      p->info.subscribed = false;
    }
    p->track->transition(TrackState::closed);
    p->emit(RoomEventKind::track_unpublished);
    engine_->defer([p](RtcSession& s, const Operation& op) {
      auto error = s.subscribe(p->info.sid, p->track, false, op);
      if (!error) {
        LOG_ERROR("LiveKitClient", "detach removed track: " << error.error().message);
      }
    });
  }
  for (auto& p : discovered) {
    p->emit(RoomEventKind::track_published);
    if (p->info.desired && !engine_->defer([p](RtcSession& s, const Operation& op) {
          auto error = p->subscribe(s, op, true);
          if (!error) {
            LOG_ERROR("LiveKitClient", "auto subscribe: " << error.error().message);
          }
        })) {
      LOG_ERROR("LiveKitClient", "Auto subscription queue full");
    }
  }
}

void RoomSession::on_event(SessionEvent event) {
  if (event.kind == SessionEvent::Kind::feedback || event.kind == SessionEvent::Kind::mute) {
    if (local_) {
      local_->on_event(event);
    }
    return;
  }
  if (event.kind == SessionEvent::Kind::participant) {
    participant(std::move(event.participant));
  } else if (event.kind == SessionEvent::Kind::room) {
    std::lock_guard lock(mutex_);
    info_ = std::move(event.room);
  } else if (event.kind == SessionEvent::Kind::data) {
    RoomEvent value;
    value.kind = RoomEventKind::data_received;
    value.data = std::move(event.data);
    value.participant_identity = std::move(event.identity);
    notify(std::move(value));
  }
}

void RoomSession::notify(RoomEvent event) {
  std::lock_guard lock(notification_mutex_);
  if (!observer_ || notifications_closed_) {
    return;
  }
  size_t bytes = event.data ? event.data->payload.size() : 0;
  if (notifications_.size() >= max_events_ || notification_bytes_ + bytes > 4 * 1024 * 1024) {
    LOG_ERROR("LiveKitClient", "Observer queue overflow; closing session");
    // Never silently lose a state change. Request cancellation without joining
    // the engine from its own event callback.
    notifications_.clear();
    notification_bytes_ = 0;
    RoomEvent failure;
    failure.kind = RoomEventKind::connection_changed;
    failure.connection = ConnectionState::failed;
    failure.failure = make_failure(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                                   "Observer queue overflow");
    engine_->request_stop(failure.failure);
    notifications_.push_back(std::move(failure));
  } else {
    notification_bytes_ += bytes;
    notifications_.push_back(std::move(event));
  }
  notification_changed_.notify_all();
}

void RoomSession::run_notifications() {
  sdk_thread = true;
  std::unique_lock lock(notification_mutex_);
  for (;;) {
    notification_changed_.wait(lock,
                               [&] { return notifications_closed_ || !notifications_.empty(); });
    if (notifications_closed_) {
      break;
    }
    auto event = std::move(notifications_.front());
    notifications_.pop_front();
    notification_bytes_ -= event.data ? event.data->payload.size() : 0;
    auto observer = observer_;
    notifying_ = observer != nullptr;
    lock.unlock();
    if (observer) {
      observer->on_room_event(event);
    }
    lock.lock();
    notifying_ = false;
    notification_changed_.notify_all();
  }
  sdk_thread = false;
}
}  // namespace livekit::sdk::client::detail
