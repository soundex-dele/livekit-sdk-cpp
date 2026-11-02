#include "backends/rtc/room_engine.hpp"
#include "backends/rtc/endpoint.hpp"
#include "backends/rtc/track_control.hpp"
#include "backends/rtc/execution.hpp"
#include <utility>
#include <algorithm>

namespace livekit::sdk::rtc {
using detail::BindingToken;
using detail::Endpoint;
using detail::TransportEventKind;

void RoomEngine::event(RoomEvent value) {
  value.epoch = status_.epoch;
  notify([value = std::move(value)](auto& observer) { observer.on_room_event(value); });
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
  auto it = tracks_.find({endpoint->descriptor.id, false});
  if (it == tracks_.end() || it->second.endpoint != endpoint) {
    return;
  }
  auto& entry = it->second;
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
  auto entry = std::move(it->second);
  tracks_.erase(it);
  entry.endpoint->finish(TrackState::closed);
  // Notify dependent media owners before completions can reuse this track id.
  RoomEvent value{RoomEventKind::track_removed, binding.id};
  value.publication = binding.publication;
  event(std::move(value));
  auto cancelled = detail::failure_result<void>(ErrorCode::cancelled, "Track removed before ready");
  complete(entry.publish_done,
           Result<std::shared_ptr<PublishedTrack>>::failure(cancelled.failure_record()));
  complete(entry.subscribe_done,
           Result<std::shared_ptr<SubscribedTrack>>::failure(cancelled.failure_record()));
  for (auto it = control_completions_.begin(); it != control_completions_.end();) {
    auto& operation = it->second;
    if (operation.epoch == binding.epoch && operation.track == binding.id &&
        operation.publication == binding.publication) {
      auto done = std::move(operation.callback);
      it = control_completions_.erase(it);
      complete(done,
               detail::failure_result<void>(ErrorCode::cancelled, "Track removed during control"));
    } else {
      ++it;
    }
  }
  for (auto& done : entry.remove_done) {
    complete(done, Result<void>::success());
  }
}

void RoomEngine::remove(const TrackId& id, bool publication, Completion<void> completion) {
  auto it = tracks_.find({id, publication});
  if (it == tracks_.end()) {
    complete(completion,
             detail::failure_result<void>(ErrorCode::invalid_argument, "Unknown track"));
    return;
  }
  auto& entry = it->second;
  if (entry.removing) {
    if (completion) {
      entry.remove_done.push_back(std::move(completion));
    }
    return;
  }
  entry.removing = true;
  auto endpoint = entry.endpoint;
  auto submitted = [&] {
    return transport_->remove(endpoint->binding(), publication, [this, endpoint](Failure failure) {
      execution_->control.post([this, endpoint, failure] {
        if (!transport_) {
          return;
        }
        if (!failure) {
          removed(endpoint->binding());
          return;
        }
        auto it = tracks_.find({endpoint->descriptor.id, endpoint->publication});
        if (it == tracks_.end() || it->second.endpoint != endpoint) {
          return;
        }
        auto callbacks = std::move(it->second.remove_done);
        it->second.removing = false;
        for (auto& done : callbacks) {
          complete(done, Result<void>::failure(failure));
        }
      });
    });
  }();
  if (!submitted) {
    entry.removing = false;
    complete(completion, std::move(submitted));
    return;
  }
  if (completion) {
    entry.remove_done.push_back(std::move(completion));
  }
}

void RoomEngine::deliver(std::function<void()> callback) {
  execution_->notification.post([callback = std::move(callback)] {
    detail::CallbackScope scope;
    callback();
  });
}

void RoomEngine::fail_pending(std::shared_ptr<const FailureInfo> failure) {
  complete(join_completion_, Result<RoomSnapshot>::failure(failure));
  for (auto& [key, entry] : tracks_) {
    complete(entry.publish_done, Result<std::shared_ptr<PublishedTrack>>::failure(failure));
    complete(entry.subscribe_done, Result<std::shared_ptr<SubscribedTrack>>::failure(failure));
    auto callbacks = std::move(entry.remove_done);
    for (auto& done : callbacks) {
      complete(done, Result<void>::failure(failure));
    }
  }
  auto controls = std::move(control_completions_);
  control_completions_.clear();
  for (auto& [id, operation] : controls) {
    complete(operation.callback, Result<void>::failure(failure));
  }
}

RoomEngine::RoomEngine(TransportFactory factory)
    : execution_(std::make_shared<detail::RoomExecution>()),
      factory_(std::move(factory)),
      commands_(std::make_shared<detail::TrackControl>()) {
  commands_->send = [this](auto endpoint) {
    execution_->control.post([this, endpoint] {
      if (transport_) {
        send(endpoint);
      }
    });
  };
  commands_->request_keyframe = [this](auto endpoint) {
    execution_->control.post([this, endpoint] {
      if (transport_) {
        request_keyframe(endpoint);
      }
    });
  };
  commands_->set_muted = [this](const auto& id, bool muted) { set_muted(id, muted); };
  commands_->set_receive_demand = [this](const auto& id, auto demand) {
    set_receive_demand(id, demand);
  };
  commands_->close_track = [this](auto endpoint) {
    execution_->control.post([this, endpoint] {
      if (transport_ && endpoint->snapshot().state != TrackState::closed) {
        remove(endpoint->descriptor.id, endpoint->publication);
      }
    });
  };
}

RoomEngine::~RoomEngine() {
  (void)set_observer(nullptr);
  leave();
  {
    std::lock_guard admission(request_mutex_);
    closing_ = true;
  }
  commands_.reset();
  execution_->control.fence();
}

void RoomEngine::notify(std::function<void(RoomObserver&)> fn) {
  if (observer_) {
    deliver([observer = observer_, fn = std::move(fn)] { fn(*observer); });
  }
}

ErrorCode RoomEngine::set_observer(RoomObserver* observer) {
  if (!execution_->external()) {
    return ErrorCode::invalid_thread;
  }
  execution_->control.invoke([this, observer] { observer_ = observer; });
  execution_->notification.fence();
  return ErrorCode::ok;
}

template <class T, class F>
void RoomEngine::post_request(Completion<T> completion, F request) {
  {
    // Serialize admission with leave, without holding a lock while running work.
    std::lock_guard lock(request_mutex_);
    if (!closing_) {
      execution_->control.post(
          [this, completion = std::move(completion), request = std::move(request)]() mutable {
            if (closing_) {
              auto cancelled = detail::failure_result<T>(ErrorCode::cancelled, "Engine is leaving");
              complete(completion, std::move(cancelled));
              return;
            }
            request(std::move(completion));
          });
      return;
    }
  }
  complete(completion, detail::failure_result<T>(ErrorCode::session_closed, "Engine is leaving"));
}

void RoomEngine::join(RoomOptions options, Completion<RoomSnapshot> completion) {
  post_request(
      std::move(completion), [this, options = std::move(options)](auto completion) mutable {
        if (status_.state != ConnectionState::disconnected) {
          complete(completion, detail::failure_result<RoomSnapshot>(ErrorCode::invalid_state,
                                                                    "Leave before joining again"));
          return;
        }
        auto started = [&]() -> Result<void> {
          transport_ = factory_();
          if (!transport_) {
            return detail::failure_result<void>(ErrorCode::internal_error,
                                                "Transport factory returned null");
          }
          transport_->set_event_handler([this](auto events) {
            execution_->control.post([this, events = std::move(events)]() mutable {
              if (transport_) {
                on_events(std::move(events));
              }
            });
          });
          if (auto attached = transport_->attach_notifications([](auto task) { task(); }, {});
              !attached) {
            return attached;
          }
          budget_ = std::make_shared<detail::QueueBudget>(options.max_queued_bytes);
          transport_->set_queue_budget(budget_);
          ++status_.epoch;
          status_.state = ConnectionState::connecting;
          status_.capabilities = transport_->capabilities();
          return transport_->start(options, status_.epoch);
        }();
        if (!started) {
          status_.state = ConnectionState::failed;
          complete(completion, Result<RoomSnapshot>::failure(started.failure_record()));
          return;
        }
        join_completion_ = std::move(completion);
      });
}

void RoomEngine::leave(Completion<void> completion) {
  if (!execution_->external()) {
    complete(completion, detail::failure_result<void>(ErrorCode::invalid_thread,
                                                      "Leave requires an external owner thread"));
    return;
  }
  std::lock_guard lock(leave_mutex_);
  {
    std::lock_guard admission(request_mutex_);
    closing_ = true;
  }
  auto transport = execution_->control.invoke([this] {
    fail_pending(
        detail::failure_result<void>(ErrorCode::cancelled, "Room is leaving").failure_record());
    for (auto& [key, entry] : tracks_) {
      entry.endpoint->finish(TrackState::closed);
    }
    tracks_.clear();
    remote_tracks_.clear();
    return std::move(transport_);
  });
  if (transport) {
    transport->stop();
  }
  execution_->control.invoke(
      [this, had_transport = bool(transport), completion = std::move(completion)]() mutable {
        status_.state = ConnectionState::disconnected;
        status_.session.reset();
        complete(completion, Result<void>::success());
        if (had_transport) {
          event({RoomEventKind::closed});
        }
      });
  execution_->notification.fence();
  {
    std::lock_guard admission(request_mutex_);
    closing_ = false;
  }
}

void RoomEngine::publish(TrackDescriptor descriptor,
                         Completion<std::shared_ptr<PublishedTrack>> completion) {
  post_request(std::move(completion), [this, descriptor =
                                                 std::move(descriptor)](auto completion) mutable {
    using Value = std::shared_ptr<PublishedTrack>;
    if (!transport_ || status_.state != ConnectionState::connected) {
      complete(completion,
               detail::failure_result<Value>(ErrorCode::invalid_state, "Room is not connected"));
      return;
    }
    const auto key = std::make_pair(descriptor.id, true);
    if (tracks_.count(key)) {
      complete(completion, detail::failure_result<Value>(ErrorCode::busy, "Track already exists"));
      return;
    }
    auto started = [&]() -> Result<void> {
      if (auto valid = detail::validate_descriptor(descriptor, budget_->limit, true); !valid) {
        return valid;
      }
      auto endpoint = make_endpoint(std::move(descriptor), true);
      auto track = Value(new PublishedTrack(endpoint, commands_));
      tracks_.emplace(key, Entry{endpoint, track, {}});
      return transport_->publish(endpoint->binding(), endpoint->descriptor);
    }();
    if (!started) {
      tracks_.erase(key);
      complete(completion, Result<Value>::failure(started.failure_record()));
      return;
    }
    tracks_.find(key)->second.publish_done = std::move(completion);
  });
}

void RoomEngine::subscribe(TrackDescriptor descriptor,
                           Completion<std::shared_ptr<SubscribedTrack>> completion) {
  post_request(std::move(completion),
               [this, descriptor = std::move(descriptor)](auto completion) mutable {
                 subscribe_on_control(std::move(descriptor), std::move(completion));
               });
}

void RoomEngine::subscribe_on_control(TrackDescriptor descriptor,
                                      Completion<std::shared_ptr<SubscribedTrack>> completion) {
  using Value = std::shared_ptr<SubscribedTrack>;
  if (!transport_ || status_.state != ConnectionState::connected) {
    complete(completion,
             detail::failure_result<Value>(ErrorCode::invalid_state, "Room is not connected"));
    return;
  }
  const auto key = std::make_pair(descriptor.id, false);
  if (tracks_.count(key)) {
    complete(completion, detail::failure_result<Value>(ErrorCode::busy, "Track already exists"));
    return;
  }
  auto started = [&]() -> Result<void> {
    if (auto valid = detail::validate_descriptor(descriptor, budget_->limit, false); !valid) {
      return valid;
    }
    auto endpoint = make_endpoint(std::move(descriptor), false);
    auto track = Value(new SubscribedTrack(endpoint, commands_));
    tracks_.emplace(key, Entry{endpoint, {}, track});
    return transport_->subscribe(endpoint->binding(), endpoint->descriptor,
                                 endpoint->queue_budget());
  }();
  if (!started) {
    tracks_.erase(key);
    complete(completion, Result<Value>::failure(started.failure_record()));
    return;
  }
  tracks_.find(key)->second.subscribe_done = std::move(completion);
}

void RoomEngine::subscribe(const TrackId& id,
                           Completion<std::shared_ptr<SubscribedTrack>> completion) {
  post_request(std::move(completion), [this, id](auto completion) mutable {
    auto it = remote_tracks_.find(id);
    if (it == remote_tracks_.end()) {
      complete(completion, detail::failure_result<std::shared_ptr<SubscribedTrack>>(
                               ErrorCode::invalid_argument, "Unknown remote track"));
      return;
    }
    subscribe_on_control(it->second, std::move(completion));
  });
}

void RoomEngine::unpublish(const TrackId& id, Completion<void> completion) {
  post_request(std::move(completion),
               [this, id](auto completion) mutable { remove(id, true, std::move(completion)); });
}

void RoomEngine::unsubscribe(const TrackId& id, Completion<void> completion) {
  post_request(std::move(completion),
               [this, id](auto completion) mutable { remove(id, false, std::move(completion)); });
}

void RoomEngine::set_muted(const TrackId& id, bool muted, Completion<void> completion) {
  post_request(std::move(completion), [this, id, muted](auto completion) mutable {
    if (!transport_ || status_.state != ConnectionState::connected) {
      complete(completion,
               detail::failure_result<void>(ErrorCode::invalid_state, "Room is not connected"));
      return;
    }
    auto it = tracks_.find({id, true});
    if (it == tracks_.end()) {
      complete(completion,
               detail::failure_result<void>(ErrorCode::invalid_argument, "Unknown track"));
      return;
    }
    const auto serial = ++next_control_;
    auto submitted = [&]() -> Result<void> {
      auto endpoint = it->second.endpoint;
      endpoint->set_muted(muted);
      return transport_->set_muted(
          {serial, endpoint->binding(), muted, SteadyClock::time_point::max(), {}});
    }();
    if (!submitted) {
      complete(completion, std::move(submitted));
    } else if (completion) {
      control_completions_.emplace(
          serial, ControlCompletion{status_.epoch, id, true, std::move(completion)});
    }
  });
}

void RoomEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                    Completion<void> completion) {
  post_request(std::move(completion), [this, id, demand](auto completion) mutable {
    if (!transport_ || status_.state != ConnectionState::connected) {
      complete(completion,
               detail::failure_result<void>(ErrorCode::invalid_state, "Room is not connected"));
      return;
    }
    auto it = tracks_.find({id, false});
    if (it == tracks_.end()) {
      complete(completion,
               detail::failure_result<void>(ErrorCode::invalid_argument, "Unknown track"));
      return;
    }
    const auto serial = ++next_control_;
    auto submitted = [&]() -> Result<void> {
      auto endpoint = it->second.endpoint;
      if (auto valid = detail::validate_receive_demand(demand, endpoint->descriptor.kind); !valid) {
        return valid;
      }
      endpoint->set_receive_demand(demand);
      return transport_->set_receive_demand(
          {serial, endpoint->binding(), demand, SteadyClock::time_point::max(), {}});
    }();
    if (!submitted) {
      complete(completion, std::move(submitted));
    } else if (completion) {
      control_completions_.emplace(
          serial, ControlCompletion{status_.epoch, id, false, std::move(completion)});
    }
  });
}

void RoomEngine::update_local_participant(ParticipantUpdate update, Completion<void> completion) {
  post_request(
      std::move(completion), [this, update = std::move(update)](auto completion) mutable {
        if (!transport_ || status_.state != ConnectionState::connected) {
          complete(completion,
                   detail::failure_result<void>(ErrorCode::invalid_state, "Room is not connected"));
          return;
        }

        const auto serial = ++next_control_;
        auto submitted = [&]() -> Result<void> {
          return transport_->update_participant(
              {serial, status_.epoch, std::move(update), SteadyClock::time_point::max(), {}});
        }();
        if (!submitted) {
          complete(completion, std::move(submitted));
        } else if (completion) {
          control_completions_.emplace(
              serial, ControlCompletion{status_.epoch, {}, true, std::move(completion)});
        }
      });
}

void RoomEngine::set_subscription_permissions(SubscriptionPermissions permissions,
                                              Completion<void> completion) {
  post_request(
      std::move(completion), [this, permissions = std::move(permissions)](auto completion) mutable {
        if (!transport_ || status_.state != ConnectionState::connected) {
          complete(completion,
                   detail::failure_result<void>(ErrorCode::invalid_state, "Room is not connected"));
          return;
        }

        const auto serial = ++next_control_;
        auto submitted = [&]() -> Result<void> {
          return transport_->set_subscription_permissions(
              {serial, status_.epoch, std::move(permissions), SteadyClock::time_point::max(), {}});
        }();
        if (!submitted) {
          complete(completion, std::move(submitted));
        } else if (completion) {
          control_completions_.emplace(
              serial, ControlCompletion{status_.epoch, {}, true, std::move(completion)});
        }
      });
}

void RoomEngine::on_events(std::vector<detail::TransportEvent> events) {
  // A join batch includes the initial roster. Make it visible to the join completion.
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
        complete(join_completion_, Result<RoomSnapshot>::success(snapshot()));
        this->event({RoomEventKind::connected});
        break;
      case TransportEventKind::control_completed: {
        auto it = control_completions_.find(event.operation_id);
        if (it == control_completions_.end() ||
            (event.binding.epoch && event.binding.epoch != it->second.epoch)) {
          break;
        }
        auto done = std::move(it->second.callback);
        control_completions_.erase(it);
        if (event.control_error && *event.control_error != ErrorCode::ok) {
          auto failure = std::make_shared<FailureInfo>(
              FailureInfo{next_failure_id(), ErrorDomain::protocol, *event.control_error,
                          event.error, false, event.control_reason});
          complete(done, Result<void>::failure(std::move(failure)));
        } else {
          complete(done, Result<void>::success());
        }
        break;
      }
      case TransportEventKind::interrupted:
      case TransportEventKind::terminal_error: {
        const bool terminal = event.kind == TransportEventKind::terminal_error;
        auto failure = event.failure
                           ? event.failure
                           : detail::failure_result<void>(ErrorCode::connection_lost, event.error)
                                 .failure_record();
        fail_pending(failure);
        status_.state = terminal ? ConnectionState::failed : ConnectionState::reconnecting;
        status_.error = failure->message;
        status_.session.reset();
        RoomEvent value{terminal ? RoomEventKind::failed : RoomEventKind::reconnecting};
        value.failure = failure;
        value.message = failure->message;
        this->event(std::move(value));
        break;
      }
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
            complete(entry.publish_done,
                     Result<std::shared_ptr<PublishedTrack>>::success(entry.published));
            break;
          case TransportEventKind::subscribed:
            endpoint->ready(event.server_sid);
            request_keyframe(endpoint);
            complete(entry.subscribe_done,
                     Result<std::shared_ptr<SubscribedTrack>>::success(entry.subscribed));
            break;
          case TransportEventKind::operation_failed: {
            auto failure =
                event.failure ? event.failure
                              : detail::failure_result<void>(ErrorCode::request_failed, event.error)
                                    .failure_record();
            complete(entry.publish_done, Result<std::shared_ptr<PublishedTrack>>::failure(failure));
            complete(entry.subscribe_done,
                     Result<std::shared_ptr<SubscribedTrack>>::failure(failure));
            auto callbacks = std::move(entry.remove_done);
            entry.removing = false;
            for (auto& done : callbacks) {
              complete(done, Result<void>::failure(failure));
            }
            RoomEvent value{RoomEventKind::track_failed, event.binding.id};
            value.publication = event.binding.publication;
            value.failure = failure;
            value.message = failure->message;
            this->event(std::move(value));
            break;
          }
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
  return execution_->control.invoke([this] {
    auto result = status_;
    result.publications = result.subscriptions = 0;
    for (const auto& [key, entry] : tracks_) {
      key.second ? ++result.publications : ++result.subscriptions;
    }
    result.queued_bytes = budget_ ? budget_->used.load() : 0;
    return result;
  });
}

std::shared_ptr<RoomServices> RoomEngine::services() const {
  return execution_->control.invoke(
      [this] { return transport_ ? transport_->services() : nullptr; });
}
}  // namespace livekit::sdk::rtc
