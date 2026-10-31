#include "backends/rtc/room_engine.hpp"
#include "backends/rtc/endpoint.hpp"
#include "backends/rtc/track_control.hpp"
#include <utility>
#include <algorithm>

namespace livekit::sdk::rtc {
using detail::BindingToken;
using detail::Endpoint;
using detail::TransportEventKind;

void RoomEngine::event(RoomEvent value) {
  value.epoch = status_.epoch;
  if (observer_) {
    observer_->on_room_event(value);
  }
}

std::shared_ptr<Endpoint> RoomEngine::make_endpoint(TrackDescriptor descriptor, bool publication) {
  BindingToken binding{descriptor.id, status_.epoch, ++next_binding_, publication};
  auto endpoint = std::make_shared<Endpoint>(std::move(descriptor), binding, budget_);
  return endpoint;
}

void RoomEngine::send(const std::shared_ptr<Endpoint>& endpoint) {
  while (auto frame = endpoint->front()) {
    const auto result = transport_->send(endpoint->binding(), frame->frame);
    if (result == detail::SendResult::would_block) {
      return;
    }
    endpoint->consumed(frame->serial, result == detail::SendResult::dropped);
  }
}

void RoomEngine::request_keyframe(const std::shared_ptr<Endpoint>& endpoint) {
  if (endpoint->snapshot().state != TrackState::ready) {
    return;
  }
  auto& entry = tracks_.at({endpoint->descriptor.id, false});
  for (const auto& [id, encoding] : endpoint->feedback().encodings) {
    if (encoding.keyframe_required &&
        (!entry.keyframe_requests || encoding.keyframe_requests > *entry.keyframe_requests)) {
      entry.keyframe_requests = encoding.keyframe_requests;
      transport_->request_keyframe(endpoint->binding());
      return;
    }
  }
}

void RoomEngine::removed(const BindingToken& binding) {
  auto it = tracks_.find({binding.id, binding.publication});
  if (it == tracks_.end() || !it->second.endpoint->matches(binding)) {
    return;
  }
  it->second.endpoint->finish(TrackState::closed);
  tracks_.erase(it);
  if (observer_) {
    if (binding.publication) {
      observer_->on_unpublished(binding.id);
    } else {
      observer_->on_unsubscribed(binding.id);
    }
  }
}

void RoomEngine::remove(const TrackId& id, bool publication) {
  auto& entry = tracks_.at({id, publication});
  if (entry.removing) {
    return;
  }
  entry.removing = true;
  auto endpoint = entry.endpoint;
  transport_->remove(endpoint->binding(), publication,
                     [this, endpoint](std::exception_ptr failure) {
                       if (!failure) {
                         removed(endpoint->binding());
                       }
                     });
}

RoomEngine::RoomEngine(TransportFactory factory)
    : factory_(std::move(factory)), commands_(std::make_shared<detail::TrackControl>()) {
  commands_->send = [this](auto endpoint) { send(endpoint); };
  commands_->request_keyframe = [this](auto endpoint) { request_keyframe(endpoint); };
  commands_->set_muted = [this](const auto& id, bool muted) { set_muted(id, muted); };
  commands_->set_receive_demand = [this](const auto& id, auto demand) {
    set_receive_demand(id, demand);
  };
  commands_->close_track = [this](auto endpoint) {
    if (endpoint->snapshot().state == TrackState::closed) {
      return;
    }
    remove(endpoint->descriptor.id, endpoint->publication);
  };
}

RoomEngine::~RoomEngine() {
  set_observer(nullptr);
  leave();
}

void RoomEngine::set_observer(RoomObserver* observer) {
  observer_ = observer;
}

void RoomEngine::join(RoomOptions options) {
  transport_ = factory_();
  transport_->set_event_handler([this](auto events) { on_events(std::move(events)); });
  transport_->attach_notifications([](auto task) { task(); }, {});
  budget_ = std::make_shared<detail::QueueBudget>(options.max_queued_bytes);
  transport_->set_queue_budget(budget_);
  ++status_.epoch;
  status_.state = ConnectionState::connecting;
  status_.capabilities = transport_->capabilities();
  transport_->start(options, status_.epoch);
}

void RoomEngine::leave() {
  if (!transport_) {
    return;
  }
  transport_->stop();
  for (auto& [key, entry] : tracks_) {
    entry.endpoint->finish(TrackState::closed);
  }
  tracks_.clear();
  remote_tracks_.clear();
  transport_.reset();
  status_.state = ConnectionState::disconnected;
  status_.session.reset();
  if (observer_) {
    observer_->on_left();
  }
}

void RoomEngine::publish(TrackDescriptor descriptor) {
  auto endpoint = make_endpoint(std::move(descriptor), true);
  auto track = std::shared_ptr<PublishedTrack>(new PublishedTrack(endpoint, commands_));
  tracks_.emplace(std::make_pair(endpoint->descriptor.id, true), Entry{endpoint, track, {}});
  transport_->publish(endpoint->binding(), endpoint->descriptor);
}

void RoomEngine::subscribe(TrackDescriptor descriptor) {
  auto endpoint = make_endpoint(std::move(descriptor), false);
  auto track = std::shared_ptr<SubscribedTrack>(new SubscribedTrack(endpoint, commands_));
  tracks_.emplace(std::make_pair(endpoint->descriptor.id, false), Entry{endpoint, {}, track});
  transport_->subscribe(endpoint->binding(), endpoint->descriptor, endpoint->queue_budget());
}

void RoomEngine::subscribe(const TrackId& id) {
  subscribe(remote_tracks_.at(id));
}

void RoomEngine::unpublish(const TrackId& id) {
  remove(id, true);
}

void RoomEngine::unsubscribe(const TrackId& id) {
  remove(id, false);
}

void RoomEngine::set_muted(const TrackId& id, bool muted) {
  auto endpoint = tracks_.at({id, true}).endpoint;
  endpoint->set_muted(muted);
  transport_->set_muted(
      {++next_control_, endpoint->binding(), muted, SteadyClock::time_point::max(), {}});
}

void RoomEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand) {
  auto endpoint = tracks_.at({id, false}).endpoint;
  endpoint->set_receive_demand(demand);
  transport_->set_receive_demand(
      {++next_control_, endpoint->binding(), demand, SteadyClock::time_point::max(), {}});
}

void RoomEngine::update_local_participant(ParticipantUpdate update) {
  transport_->update_participant(
      {++next_control_, status_.epoch, std::move(update), SteadyClock::time_point::max(), {}});
}

void RoomEngine::set_subscription_permissions(SubscriptionPermissions permissions) {
  transport_->set_subscription_permissions(
      {++next_control_, status_.epoch, std::move(permissions), SteadyClock::time_point::max(), {}});
}

void RoomEngine::on_events(std::vector<detail::TransportEvent> events) {
  // A join batch includes the initial roster. Make it visible to on_joined.
  for (const auto& event : events) {
    if (event.session) {
      status_.session = event.session;
    }
  }
  // Native join batches can put discovery before connected. Publish join success
  // first, so applications can subscribe from discovery callbacks after joining.
  std::stable_partition(events.begin(), events.end(), [](const auto& event) {
    return event.kind == TransportEventKind::connected;
  });
  for (auto& event : events) {
    if (!transport_) {
      break;
    }
    switch (event.kind) {
      case TransportEventKind::connected:
        status_.state = ConnectionState::connected;
        if (observer_) {
          observer_->on_joined(snapshot());
        }
        break;
      case TransportEventKind::remote_track_added:
        remote_tracks_[event.descriptor->id] = *event.descriptor;
        this->event({RoomEventKind::remote_track_added, event.descriptor->id, event.descriptor});
        break;
      case TransportEventKind::remote_track_removed:
        remote_tracks_.erase(event.binding.id);
        this->event({RoomEventKind::remote_track_removed, event.binding.id, event.descriptor});
        break;
      case TransportEventKind::session_changed:
        this->event({RoomEventKind::session_changed});
        break;
      case TransportEventKind::encryption_state_changed: {
        RoomEvent value{RoomEventKind::encryption_state_changed};
        value.encryption = event.encryption;
        this->event(std::move(value));
        break;
      }
      case TransportEventKind::subscription_permission_changed: {
        RoomEvent value{RoomEventKind::subscription_permission_changed};
        value.permission = event.permission;
        this->event(std::move(value));
        break;
      }
      case TransportEventKind::mute_changed: {
        RoomEvent value{RoomEventKind::track_mute_changed, event.binding.id, event.descriptor};
        value.publication = event.binding.publication;
        this->event(std::move(value));
        break;
      }
      case TransportEventKind::statistics: {
        RoomEvent value{RoomEventKind::statistics};
        value.statistics = std::move(event.statistics);
        this->event(std::move(value));
        break;
      }
      default: {
        auto it = tracks_.find({event.binding.id, event.binding.publication});
        if (it == tracks_.end()) {
          break;
        }
        auto& entry = it->second;
        auto endpoint = entry.endpoint;
        if (!endpoint->matches(event.binding)) {
          break;
        }
        switch (event.kind) {
          case TransportEventKind::published:
            endpoint->ready(event.server_sid);
            if (observer_) {
              observer_->on_published(entry.published);
            }
            break;
          case TransportEventKind::subscribed:
            endpoint->ready(event.server_sid);
            request_keyframe(endpoint);
            if (observer_) {
              observer_->on_subscribed(entry.subscribed);
            }
            break;
          case TransportEventKind::frame:
            endpoint->write(std::move(*event.frame));
            request_keyframe(endpoint);
            break;
          case TransportEventKind::feedback:
            endpoint->update_feedback(*event.feedback);
            break;
          case TransportEventKind::writable:
            if (!entry.removing) {
              send(endpoint);
            }
            break;
          case TransportEventKind::removed:
            removed(event.binding);
            break;
          default:
            break;
        }
        break;
      }
    }
  }
}

RoomSnapshot RoomEngine::snapshot() const {
  auto result = status_;
  result.publications = result.subscriptions = 0;
  for (const auto& [key, entry] : tracks_) {
    key.second ? ++result.publications : ++result.subscriptions;
  }
  result.queued_bytes = budget_ ? budget_->used.load() : 0;
  return result;
}

std::shared_ptr<RoomServices> RoomEngine::services() const {
  return transport_ ? transport_->services() : nullptr;
}
}  // namespace livekit::sdk::rtc
