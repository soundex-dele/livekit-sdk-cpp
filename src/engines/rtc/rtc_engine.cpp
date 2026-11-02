#include "backends/rtc/legacy_control_boundary.hpp"
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "engines/rtc/media_graph_contract.hpp"
#include "backends/rtc/execution.hpp"

namespace livekit::sdk::rtc::engine {
RtcEngine::RtcEngine(TransportFactory factory)
    : room_(std::make_unique<RoomEngine>(std::move(factory))),
      execution_(room_->execution_),
      lifecycle_(std::make_unique<detail::SerialQueue>("rtc.lifecycle")) {
  (void)room_->set_observer(this);
}

RtcEngine::~RtcEngine() {
  (void)set_observer(nullptr);
  leave();
  {
    std::lock_guard admission(request_mutex_);
    closing_ = true;
  }
  (void)room_->set_observer(nullptr);
  // Observer removal fences notification; drain work already handed to lifecycle.
  lifecycle_->invoke([this] {
    media_.reset();
    media_resources_.reset();
  });
  execution_->notification.fence();
}

ErrorCode RtcEngine::set_observer(RtcObserver* observer) {
  if (!execution_->external()) {
    return ErrorCode::invalid_thread;
  }
  execution_->notification.invoke([this, observer] { observer_ = observer; });
  return ErrorCode::ok;
}

void RtcEngine::deliver(std::function<void()> callback) {
  execution_->notification.post([callback = std::move(callback)] {
    detail::CallbackScope scope;
    callback();
  });
}

void RtcEngine::notify(std::function<void(RtcObserver&)> fn) {
  deliver([this, fn = std::move(fn)] {
    if (observer_) {
      fn(*observer_);
    }
  });
}

template <class T, class F>
void RtcEngine::post_request(Completion<T> completion, F request) {
  {
    // Serialize admission with leave, without holding a lock while running work.
    std::lock_guard lock(request_mutex_);
    if (!closing_) {
      lifecycle_->post(
          [this, completion = std::move(completion), request = std::move(request)]() mutable {
            if (closing_) {
              auto cancelled = detail::failure_result<T>(ErrorCode::cancelled, "Engine is leaving");
              complete(std::move(completion), std::move(cancelled));
              return;
            }
            request(std::move(completion));
          });
      return;
    }
  }
  complete(std::move(completion),
           detail::failure_result<T>(ErrorCode::session_closed, "Engine is leaving"));
}

template <class T, class F>
void RtcEngine::post_room_request(Completion<T> completion, F enqueue) {
  {
    std::lock_guard lock(request_mutex_);
    if (!closing_) {
      // Room request methods only enqueue work; control never waits for media.
      enqueue(std::move(completion));
      return;
    }
  }
  complete(std::move(completion),
           detail::failure_result<T>(ErrorCode::session_closed, "RTC is leaving"));
}

void RtcEngine::join(RoomOptions options, Completion<RoomSnapshot> completion) {
  post_room_request(std::move(completion),
                    [this, options = std::move(options)](auto completion) mutable {
                      room_->join(std::move(options), std::move(completion));
                    });
}

Result<void> RtcEngine::clear_tracks() {
  auto result = Result<void>::success();
  {
    std::lock_guard lock(ingress_mutex_);
    ingress_.clear();
  }
  for (auto& [id, entry] : publications_) {
    if (entry.pipeline) {
      auto stopped = detail::control_boundary([&]() -> Result<void> {
        (void)entry.pipeline->close();
        return Result<void>::success();
      });
      if (!stopped && result) {
        result = std::move(stopped);
      }
    }
  }
  for (auto& [id, entry] : subscriptions_) {
    if (entry.pipeline) {
      auto stopped = detail::control_boundary([&]() -> Result<void> {
        (void)entry.pipeline->close();
        return Result<void>::success();
      });
      if (!stopped && result) {
        result = std::move(stopped);
      }
    }
  }
  publications_.clear();
  subscriptions_.clear();
  return result;
}

void RtcEngine::leave(Completion<void> completion) {
  if (!execution_->external()) {
    complete(std::move(completion),
             detail::failure_result<void>(ErrorCode::invalid_thread,
                                          "Leave requires an external owner thread"));
    return;
  }
  std::lock_guard lock(leave_mutex_);
  {
    std::lock_guard admission(request_mutex_);
    closing_ = true;
  }
  auto stopped = lifecycle_->invoke([this] { return clear_tracks(); });
  room_->leave();
  lifecycle_->fence();
  complete(std::move(completion), std::move(stopped));
  execution_->notification.fence();
  {
    std::lock_guard admission(request_mutex_);
    closing_ = false;
  }
}

void RtcEngine::close(Completion<void> completion) {
  leave(std::move(completion));
}

RoomSnapshot RtcEngine::snapshot() const {
  return room_->snapshot();
}

std::shared_ptr<RoomServices> RtcEngine::services() const {
  return room_->services();
}

bool RtcEngine::media_enabled() const {
  return lifecycle_->invoke([this] { return bool(media_); });
}

void RtcEngine::publish(TrackDescriptor descriptor, Completion<MediaPublication> completion) {
  publish_graph(
      std::move(descriptor), [](auto) { return TrackGraphSpec{}; }, std::move(completion));
}

void RtcEngine::subscribe(TrackDescriptor descriptor, Completion<MediaSubscription> completion) {
  subscribe_graph(
      std::move(descriptor), [](auto) { return TrackGraphSpec{}; }, std::move(completion));
}

void RtcEngine::subscribe(const TrackId& id, Completion<MediaSubscription> completion) {
  post_request(std::move(completion), [this, id](auto completion) mutable {
    if (subscriptions_.contains(id)) {
      complete(std::move(completion), detail::failure_result<MediaSubscription>(
                                          ErrorCode::busy, "Subscription already exists"));
      return;
    }
    subscriptions_[id].factory = [](auto) { return TrackGraphSpec{}; };
    room_->subscribe(id, [this, id, completion = std::move(completion)](auto result) mutable {
      on_subscribed(id, std::move(result), std::move(completion));
    });
  });
}

void RtcEngine::publish_graph(TrackDescriptor descriptor, PublishGraphFactory factory,
                              Completion<MediaPublication> completion) {
  post_request(std::move(completion), [this, descriptor = std::move(descriptor),
                                       factory = std::move(factory)](auto completion) mutable {
    const auto id = descriptor.id;
    if (publications_.contains(id)) {
      complete(std::move(completion),
               detail::failure_result<MediaPublication>(ErrorCode::busy, "Track already exists"));
      return;
    }
    publications_[id].factory = std::move(factory);
    room_->publish(std::move(descriptor),
                   [this, id, completion = std::move(completion)](auto result) mutable {
                     on_published(id, std::move(result), std::move(completion));
                   });
  });
}

void RtcEngine::publish_media(MediaPublishSpec spec, Completion<MediaPublication> completion) {
  post_request(std::move(completion), [this, spec = std::move(spec)](auto completion) mutable {
    auto descriptor = spec.track;
    const auto id = descriptor.id;
    if (publications_.contains(id)) {
      complete(std::move(completion),
               detail::failure_result<MediaPublication>(ErrorCode::busy, "Track already exists"));
      return;
    }
    publications_[id].media = std::move(spec);
    room_->publish(std::move(descriptor),
                   [this, id, completion = std::move(completion)](auto result) mutable {
                     on_published(id, std::move(result), std::move(completion));
                   });
  });
}

void RtcEngine::subscribe_graph(TrackDescriptor descriptor, SubscribeGraphFactory factory,
                                Completion<MediaSubscription> completion) {
  post_request(std::move(completion), [this, descriptor = std::move(descriptor),
                                       factory = std::move(factory)](auto completion) mutable {
    const auto id = descriptor.id;
    if (subscriptions_.contains(id)) {
      complete(std::move(completion),
               detail::failure_result<MediaSubscription>(ErrorCode::busy, "Track already exists"));
      return;
    }
    subscriptions_[id].factory = std::move(factory);
    room_->subscribe(std::move(descriptor),
                     [this, id, completion = std::move(completion)](auto result) mutable {
                       on_subscribed(id, std::move(result), std::move(completion));
                     });
  });
}

void RtcEngine::subscribe_media(MediaSubscribeSpec spec, Completion<MediaSubscription> completion) {
  post_request(std::move(completion), [this, spec = std::move(spec)](auto completion) mutable {
    auto descriptor = spec.track;
    const auto id = descriptor.id;
    if (subscriptions_.contains(id)) {
      complete(std::move(completion),
               detail::failure_result<MediaSubscription>(ErrorCode::busy, "Track already exists"));
      return;
    }
    subscriptions_[id].media = std::move(spec);
    room_->subscribe(std::move(descriptor),
                     [this, id, completion = std::move(completion)](auto result) mutable {
                       on_subscribed(id, std::move(result), std::move(completion));
                     });
  });
}

void RtcEngine::publish_video(MediaPublishSpec spec, Completion<MediaPublication> completion) {
  spec.track.kind = MediaKind::video;
  publish_media(std::move(spec), std::move(completion));
}

void RtcEngine::publish_audio(MediaPublishSpec spec, Completion<MediaPublication> completion) {
  spec.track.kind = MediaKind::audio;
  publish_media(std::move(spec), std::move(completion));
}

Result<std::shared_ptr<SharedMediaInput>> RtcEngine::create_shared_input(TrackDescriptor descriptor,
                                                                         MediaInput input) {
  return lifecycle_->invoke(
      [this, descriptor = std::move(descriptor), input = std::move(input)]() mutable {
        return detail::control_boundary([&]() -> Result<std::shared_ptr<SharedMediaInput>> {
          if (!media_) {
            return detail::failure_result<std::shared_ptr<SharedMediaInput>>(
                ErrorCode::invalid_state, "Media is not enabled");
          }
          return media_->create_shared_input(std::move(descriptor), std::move(input));
        });
      });
}

void RtcEngine::unpublish(const TrackId& id, Completion<void> completion) {
  post_request(std::move(completion), [this, id](auto completion) mutable {
    auto it = publications_.find(id);
    if (it != publications_.end() && it->second.pipeline) {
      auto stopped = detail::control_boundary([&]() -> Result<void> {
        (void)it->second.pipeline->close();
        return Result<void>::success();
      });
      if (!stopped) {
        complete(std::move(completion), std::move(stopped));
        return;
      }
    }
    room_->unpublish(id, [this, id, completion = std::move(completion)](auto result) mutable {
      lifecycle_->post(
          [this, id, completion = std::move(completion), result = std::move(result)]() mutable {
            if (result) {
              on_unpublished(id);
            }
            complete(std::move(completion), std::move(result));
          });
    });
  });
}

void RtcEngine::unsubscribe(const TrackId& id, Completion<void> completion) {
  post_request(std::move(completion), [this, id](auto completion) mutable {
    auto it = subscriptions_.find(id);
    if (it != subscriptions_.end() && it->second.pipeline) {
      auto stopped = detail::control_boundary([&]() -> Result<void> {
        (void)it->second.pipeline->close();
        return Result<void>::success();
      });
      if (!stopped) {
        complete(std::move(completion), std::move(stopped));
        return;
      }
    }
    room_->unsubscribe(id, [this, id, completion = std::move(completion)](auto result) mutable {
      lifecycle_->post(
          [this, id, completion = std::move(completion), result = std::move(result)]() mutable {
            if (result) {
              on_unsubscribed(id);
            }
            complete(std::move(completion), std::move(result));
          });
    });
  });
}

void RtcEngine::set_muted(const TrackId& id, bool muted, Completion<void> completion) {
  post_room_request(std::move(completion), [this, id, muted](auto completion) mutable {
    room_->set_muted(id, muted, std::move(completion));
  });
}

void RtcEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                   Completion<void> completion) {
  post_room_request(std::move(completion), [this, id, demand](auto completion) mutable {
    room_->set_receive_demand(id, demand, std::move(completion));
  });
}

void RtcEngine::update_local_participant(ParticipantUpdate update, Completion<void> completion) {
  post_room_request(std::move(completion),
                    [this, update = std::move(update)](auto completion) mutable {
                      room_->update_local_participant(std::move(update), std::move(completion));
                    });
}

void RtcEngine::set_subscription_permissions(SubscriptionPermissions permissions,
                                             Completion<void> completion) {
  post_room_request(
      std::move(completion), [this, permissions = std::move(permissions)](auto completion) mutable {
        room_->set_subscription_permissions(std::move(permissions), std::move(completion));
      });
}

WriteStatus RtcEngine::write(const TrackId& id, EncodedFrame frame) {
  std::lock_guard lock(ingress_mutex_);
  auto it = ingress_.find(id);
  return it == ingress_.end() ? WriteStatus::not_ready : it->second->write(std::move(frame));
}

TrackSnapshot RtcEngine::publication(const TrackId& id) const {
  std::lock_guard lock(ingress_mutex_);
  auto it = ingress_.find(id);
  if (it == ingress_.end()) {
    TrackSnapshot value;
    value.id = id;
    value.state = TrackState::failed;
    value.failure = detail::failure_result<void>(ErrorCode::invalid_argument, "Unknown publication")
                        .failure_record();
    return value;
  }
  return it->second->node().track()->snapshot();
}

Result<SenderFeedback> RtcEngine::feedback(const TrackId& id) const {
  std::lock_guard lock(ingress_mutex_);
  auto it = ingress_.find(id);
  if (it == ingress_.end()) {
    return detail::failure_result<SenderFeedback>(ErrorCode::invalid_argument,
                                                  "Unknown publication");
  }
  return Result<SenderFeedback>::success(it->second->node().track()->feedback());
}

void RtcEngine::on_published(TrackId id, Result<std::shared_ptr<PublishedTrack>> result,
                             Completion<MediaPublication> completion) {
  lifecycle_->post([this, id = std::move(id), result = std::move(result),
                    completion = std::move(completion)]() mutable {
    if (!result) {
      publications_.erase(id);
      complete(std::move(completion), Result<MediaPublication>::failure(result.failure_record()));
      return;
    }
    auto endpoint = *result;
    auto pending = publications_.find(id);
    if (closing_ || pending == publications_.end()) {
      complete(std::move(completion),
               detail::failure_result<MediaPublication>(ErrorCode::cancelled, "RTC is leaving"));
      return;
    }
    auto& entry = pending->second;
    auto built = detail::control_boundary([&]() -> Result<void> {
      auto node = std::make_shared<nodes::RtcPublishNode>("rtc.publish." + id, endpoint);
      TrackGraphSpec spec;
      if (entry.media) {
        if (!media_) {
          return detail::failure_result<void>(ErrorCode::invalid_state, "Media is not enabled");
        }
        auto prepared = media_->build_publish(std::move(*entry.media), node);
        if (!prepared) {
          return Result<void>::failure(prepared.failure_record());
        }
        spec = std::move(prepared->graph);
        entry.track = std::move(prepared->track);
        entry.media.reset();
      } else {
        if (!entry.factory) {
          return detail::failure_result<void>(ErrorCode::invalid_argument, "Empty graph factory");
        }
        spec = entry.factory(node);
        entry.factory = {};
      }
      entry.pipeline =
          std::shared_ptr<PublishPipeline>(new PublishPipeline(node, std::move(spec), {}));
      if (auto started = entry.pipeline->start(); !started) {
        return started;
      }
      {
        std::lock_guard lock(ingress_mutex_);
        ingress_[id] = entry.pipeline;
      }
      return Result<void>::success();
    });
    if (!built) {
      rollback(id, true, built.failure_record(),
               [completion = std::move(completion)](Result<void> result) mutable {
                 if (completion) {
                   completion(Result<MediaPublication>::failure(result.failure_record()));
                 }
               });
      return;
    }
    complete(std::move(completion),
             Result<MediaPublication>::success(MediaPublication{id, entry.track}));
  });
}

void RtcEngine::on_subscribed(TrackId id, Result<std::shared_ptr<SubscribedTrack>> result,
                              Completion<MediaSubscription> completion) {
  lifecycle_->post([this, id = std::move(id), result = std::move(result),
                    completion = std::move(completion)]() mutable {
    if (!result) {
      subscriptions_.erase(id);
      complete(std::move(completion), Result<MediaSubscription>::failure(result.failure_record()));
      return;
    }
    auto endpoint = *result;
    auto pending = subscriptions_.find(id);
    if (closing_ || pending == subscriptions_.end()) {
      complete(std::move(completion),
               detail::failure_result<MediaSubscription>(ErrorCode::cancelled, "RTC is leaving"));
      return;
    }
    auto& entry = pending->second;
    auto built = detail::control_boundary([&]() -> Result<void> {
      auto node = std::make_shared<nodes::RtcSubscribeNode>("rtc.subscribe." + id, endpoint);
      TrackGraphSpec spec;
      if (entry.media) {
        if (!media_) {
          return detail::failure_result<void>(ErrorCode::invalid_state, "Media is not enabled");
        }
        auto prepared = media_->build_subscribe(std::move(*entry.media), node);
        if (!prepared) {
          return Result<void>::failure(prepared.failure_record());
        }
        spec = std::move(prepared->graph);
        entry.track = std::move(prepared->track);
        entry.media.reset();
      } else {
        if (!entry.factory) {
          return detail::failure_result<void>(ErrorCode::invalid_argument, "Empty graph factory");
        }
        spec = entry.factory(node);
        entry.factory = {};
      }
      entry.pipeline =
          std::shared_ptr<SubscribePipeline>(new SubscribePipeline(node, std::move(spec), {}));
      if (auto started = entry.pipeline->start(); !started) {
        return started;
      }

      return Result<void>::success();
    });
    if (!built) {
      rollback(id, false, built.failure_record(),
               [completion = std::move(completion)](Result<void> result) mutable {
                 if (completion) {
                   completion(Result<MediaSubscription>::failure(result.failure_record()));
                 }
               });
      return;
    }
    complete(std::move(completion),
             Result<MediaSubscription>::success(MediaSubscription{id, entry.track}));
  });
}

void RtcEngine::rollback(const TrackId& id, bool publication, Failure failure,
                         Completion<void> completion) {
  // Stop any partially prepared graph before releasing its transport endpoint.
  auto stopped = detail::control_boundary([&] {
    if (publication) {
      auto it = publications_.find(id);
      if (it != publications_.end() && it->second.pipeline) {
        (void)it->second.pipeline->close();
      }
    } else {
      auto it = subscriptions_.find(id);
      if (it != subscriptions_.end() && it->second.pipeline) {
        (void)it->second.pipeline->close();
      }
    }
  });
  if (stopped != ErrorCode::ok) {
    auto enriched = std::make_shared<FailureInfo>(*failure);
    enriched->message +=
        "; partial graph cleanup failed (code " + std::to_string(static_cast<int>(stopped)) + ")";
    failure = std::move(enriched);
  }
  Completion<void> removed = [this, id, publication, failure = std::move(failure),
                              completion = std::move(completion)](Result<void> removed) mutable {
    lifecycle_->post([this, id, publication, failure = std::move(failure),
                      completion = std::move(completion), removed = std::move(removed)]() mutable {
      if (removed) {
        if (publication) {
          on_unpublished(id);
        } else {
          on_unsubscribed(id);
        }
      } else {
        auto enriched = std::make_shared<FailureInfo>(*failure);
        enriched->message += "; endpoint rollback failed: " + removed.error().message;
        failure = std::move(enriched);
      }
      complete(std::move(completion), Result<void>::failure(std::move(failure)));
    });
  };
  if (publication) {
    room_->unpublish(id, std::move(removed));
  } else {
    room_->unsubscribe(id, std::move(removed));
  }
}

void RtcEngine::on_unpublished(const TrackId& id) {
  {
    std::lock_guard lock(ingress_mutex_);
    ingress_.erase(id);
  }
  publications_.erase(id);
}

void RtcEngine::on_unsubscribed(const TrackId& id) {
  subscriptions_.erase(id);
}

void RtcEngine::on_room_event(const RoomEvent& event) {
  if (event.kind == RoomEventKind::track_removed && event.publication) {
    lifecycle_->post([this, event] {
      if (*event.publication) {
        on_unpublished(event.track_id);
      } else {
        on_unsubscribed(event.track_id);
      }
      notify([event](auto& observer) { observer.on_room_event(event); });
    });
    return;
  }
  notify([event](auto& observer) { observer.on_room_event(event); });
}

RtcGraphApi RtcEngine::graph() {
  return RtcGraphApi(*this);
}

void RtcGraphApi::publish_graph(TrackDescriptor descriptor, PublishGraphFactory factory,
                                Completion<MediaPublication> completion) {
  engine_->publish_graph(std::move(descriptor), std::move(factory), std::move(completion));
}

void RtcGraphApi::subscribe_graph(TrackDescriptor descriptor, SubscribeGraphFactory factory,
                                  Completion<MediaSubscription> completion) {
  engine_->subscribe_graph(std::move(descriptor), std::move(factory), std::move(completion));
}

void RtcGraphApi::publish(TrackDescriptor descriptor,
                          std::shared_ptr<graphflow::core::GraphNode> sink,
                          Completion<MediaPublication> completion) {
  publish_graph(
      std::move(descriptor),
      [sink](auto node) {
        TrackGraphSpec spec;
        if (sink) {
          spec.nodes.push_back(sink);
          spec.connections.push_back(
              TrackGraphSpec::connect<nodes::SenderFeedbackEvent>(node, sink));
        }
        return spec;
      },
      std::move(completion));
}

void RtcGraphApi::subscribe(TrackDescriptor descriptor,
                            std::shared_ptr<graphflow::core::GraphNode> consumer,
                            Completion<MediaSubscription> completion) {
  subscribe_graph(
      std::move(descriptor),
      [consumer](auto node) {
        TrackGraphSpec spec;
        spec.nodes.push_back(consumer);
        spec.connections.push_back(
            TrackGraphSpec::connect<nodes::EncodedFrameEvent>(node, consumer));
        return spec;
      },
      std::move(completion));
}
}  // namespace livekit::sdk::rtc::engine
