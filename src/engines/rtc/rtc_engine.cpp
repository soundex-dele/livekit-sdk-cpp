#include "backends/rtc/legacy_control_boundary.hpp"
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "engines/rtc/media_graph_contract.hpp"
#include "backends/rtc/execution.hpp"

namespace livekit::sdk::rtc::engine {
RtcEngine::RtcEngine(TransportFactory factory)
    : execution_(std::make_shared<detail::RtcExecution>()),
      room_(new RoomEngine(std::move(factory), execution_)) {
  (void)room_->set_observer(this);
}

RtcEngine::~RtcEngine() {
  (void)set_observer(nullptr);
  leave();
  (void)room_->set_observer(nullptr);
  room_.reset();
  execution_->lifecycle.invoke([this] {
    media_.reset();
    media_resources_.reset();
  });
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

void RtcEngine::join(RoomOptions options, Completion<RoomSnapshot> completion) {
  room_->join(std::move(options), [this, completion = std::move(completion)](auto result) mutable {
    complete(std::move(completion), std::move(result));
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
        (void)entry.pipeline->close_locked();
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
        (void)entry.pipeline->close_locked();
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
  closing_ = true;
  auto stopped = execution_->lifecycle.invoke([this] { return clear_tracks(); });
  room_->leave();
  execution_->lifecycle.fence();
  closing_ = false;
  complete(std::move(completion), std::move(stopped));
  execution_->notification.fence();
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
  return execution_->lifecycle.invoke([this] { return bool(media_); });
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
  execution_->lifecycle.invoke([this, id, completion = std::move(completion)]() mutable {
    if (closing_) {
      complete(std::move(completion), detail::failure_result<MediaSubscription>(
                                          ErrorCode::session_closed, "RTC is leaving"));
      return;
    }
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
  execution_->lifecycle.invoke([this, descriptor = std::move(descriptor),
                                factory = std::move(factory),
                                completion = std::move(completion)]() mutable {
    if (closing_) {
      complete(std::move(completion), detail::failure_result<MediaPublication>(
                                          ErrorCode::session_closed, "RTC is leaving"));
      return;
    }

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
  execution_->lifecycle.invoke([this, spec = std::move(spec),
                                completion = std::move(completion)]() mutable {
    if (closing_) {
      complete(std::move(completion), detail::failure_result<MediaPublication>(
                                          ErrorCode::session_closed, "RTC is leaving"));
      return;
    }
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
  execution_->lifecycle.invoke([this, descriptor = std::move(descriptor),
                                factory = std::move(factory),
                                completion = std::move(completion)]() mutable {
    if (closing_) {
      complete(std::move(completion), detail::failure_result<MediaSubscription>(
                                          ErrorCode::session_closed, "RTC is leaving"));
      return;
    }

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
  execution_->lifecycle.invoke([this, spec = std::move(spec),
                                completion = std::move(completion)]() mutable {
    if (closing_) {
      complete(std::move(completion), detail::failure_result<MediaSubscription>(
                                          ErrorCode::session_closed, "RTC is leaving"));
      return;
    }
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
  return execution_->lifecycle.invoke(
      [this, descriptor = std::move(descriptor), input = std::move(input)]() mutable {
        return detail::control_boundary([&]() -> Result<std::shared_ptr<SharedMediaInput>> {
          if (!media_) {
            return detail::failure_result<std::shared_ptr<SharedMediaInput>>(
                ErrorCode::invalid_state, "Media is not enabled");
          }
          return Result<std::shared_ptr<SharedMediaInput>>::success(
              media_->create_shared_input(std::move(descriptor), std::move(input)));
        });
      });
}

void RtcEngine::unpublish(const TrackId& id, Completion<void> completion) {
  execution_->lifecycle.invoke([this, id, completion = std::move(completion)]() mutable {
    if (closing_) {
      complete(std::move(completion),
               detail::failure_result<void>(ErrorCode::session_closed, "RTC is leaving"));
      return;
    }
    auto it = publications_.find(id);
    if (it != publications_.end() && it->second.pipeline) {
      auto stopped = detail::control_boundary([&]() -> Result<void> {
        (void)it->second.pipeline->close_locked();
        return Result<void>::success();
      });
      if (!stopped) {
        complete(std::move(completion), std::move(stopped));
        return;
      }
    }
    room_->unpublish(id, [this, id, completion = std::move(completion)](auto result) mutable {
      execution_->lifecycle.post(
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
  execution_->lifecycle.invoke([this, id, completion = std::move(completion)]() mutable {
    if (closing_) {
      complete(std::move(completion),
               detail::failure_result<void>(ErrorCode::session_closed, "RTC is leaving"));
      return;
    }
    auto it = subscriptions_.find(id);
    if (it != subscriptions_.end() && it->second.pipeline) {
      auto stopped = detail::control_boundary([&]() -> Result<void> {
        (void)it->second.pipeline->close_locked();
        return Result<void>::success();
      });
      if (!stopped) {
        complete(std::move(completion), std::move(stopped));
        return;
      }
    }
    room_->unsubscribe(id, [this, id, completion = std::move(completion)](auto result) mutable {
      execution_->lifecycle.post(
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
  room_->set_muted(id, muted, [this, completion = std::move(completion)](auto result) mutable {
    complete(std::move(completion), std::move(result));
  });
}

void RtcEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                   Completion<void> completion) {
  room_->set_receive_demand(id, demand,
                            [this, completion = std::move(completion)](auto result) mutable {
                              complete(std::move(completion), std::move(result));
                            });
}

void RtcEngine::update_local_participant(ParticipantUpdate update, Completion<void> completion) {
  room_->update_local_participant(std::move(update),
                                  [this, completion = std::move(completion)](auto result) mutable {
                                    complete(std::move(completion), std::move(result));
                                  });
}

void RtcEngine::set_subscription_permissions(SubscriptionPermissions permissions,
                                             Completion<void> completion) {
  room_->set_subscription_permissions(
      std::move(permissions), [this, completion = std::move(completion)](auto result) mutable {
        complete(std::move(completion), std::move(result));
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
  execution_->lifecycle.post([this, id = std::move(id), result = std::move(result),
                              completion = std::move(completion)]() mutable {
    if (!result) {
      publications_.erase(id);
      complete(std::move(completion), Result<MediaPublication>::failure(result.failure_record()));
      return;
    }
    auto endpoint = *result;
    if (closing_ || !publications_.contains(id)) {
      complete(std::move(completion),
               detail::failure_result<MediaPublication>(ErrorCode::cancelled, "RTC is leaving"));
      return;
    }
    auto& entry = publications_.at(id);
    // Adapter boundary: existing graph builders and GraphEngine still use exceptions.
    auto built = detail::control_boundary([&]() -> Result<void> {
      auto node = std::make_shared<nodes::RtcPublishNode>("rtc.publish." + id, endpoint);
      TrackGraphSpec spec;
      if (entry.media) {
        if (!media_) {
          return detail::failure_result<void>(ErrorCode::invalid_state, "Media is not enabled");
        }
        auto prepared = media_->build_publish(std::move(*entry.media), node);
        spec = std::move(prepared.graph);
        entry.track = std::move(prepared.track);
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
      entry.pipeline->start_locked();
      {
        std::lock_guard lock(ingress_mutex_);
        ingress_[id] = entry.pipeline;
      }
      return Result<void>::success();
    });
    if (!built) {
      complete(std::move(completion), Result<MediaPublication>::failure(built.failure_record()));
      return;
    }
    complete(std::move(completion),
             Result<MediaPublication>::success(MediaPublication{id, entry.track}));
  });
}

void RtcEngine::on_subscribed(TrackId id, Result<std::shared_ptr<SubscribedTrack>> result,
                              Completion<MediaSubscription> completion) {
  execution_->lifecycle.post([this, id = std::move(id), result = std::move(result),
                              completion = std::move(completion)]() mutable {
    if (!result) {
      subscriptions_.erase(id);
      complete(std::move(completion), Result<MediaSubscription>::failure(result.failure_record()));
      return;
    }
    auto endpoint = *result;
    if (closing_ || !subscriptions_.contains(id)) {
      complete(std::move(completion),
               detail::failure_result<MediaSubscription>(ErrorCode::cancelled, "RTC is leaving"));
      return;
    }
    auto& entry = subscriptions_.at(id);
    // Adapter boundary: existing graph builders and GraphEngine still use exceptions.
    auto built = detail::control_boundary([&]() -> Result<void> {
      auto node = std::make_shared<nodes::RtcSubscribeNode>("rtc.subscribe." + id, endpoint);
      TrackGraphSpec spec;
      if (entry.media) {
        if (!media_) {
          return detail::failure_result<void>(ErrorCode::invalid_state, "Media is not enabled");
        }
        auto prepared = media_->build_subscribe(std::move(*entry.media), node);
        spec = std::move(prepared.graph);
        entry.track = std::move(prepared.track);
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
      entry.pipeline->start_locked();

      return Result<void>::success();
    });
    if (!built) {
      complete(std::move(completion), Result<MediaSubscription>::failure(built.failure_record()));
      return;
    }
    complete(std::move(completion),
             Result<MediaSubscription>::success(MediaSubscription{id, entry.track}));
  });
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
    execution_->lifecycle.post([this, event] {
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
