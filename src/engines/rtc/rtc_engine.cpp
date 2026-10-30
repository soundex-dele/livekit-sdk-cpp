#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "engines/rtc/rtc_engine_impl.hpp"
#include "backends/rtc/room_transport.hpp"
#include "livekit/error.hpp"

namespace livekit::sdk::rtc::engine {
namespace {
struct RoomEvents final : RoomObserver {
  explicit RoomEvents(std::weak_ptr<detail::AsyncControl> queue) : queue(std::move(queue)) {}

  void on_room_event(const RoomEvent& event) override {
    if (event.kind == RoomEventKind::session_changed || event.kind == RoomEventKind::connected) {
      if (auto worker = queue.lock()) {
        worker->request_maintenance();
      }
    }
  }

  std::weak_ptr<detail::AsyncControl> queue;
};

struct PublicationEvents final : rtc::PublicationObserver {
  explicit PublicationEvents(detail::ObserverSlot<engine::PublicationObserver>& slot)
      : slot(slot) {}

  void on_publication_event(const RoomEvent& event) override {
    if (auto observer = slot.get().lock()) {
      observer->on_publication_event(event);
    }
  }

  detail::ObserverSlot<engine::PublicationObserver>& slot;
};

struct SubscriptionEvents final : rtc::SubscriptionObserver {
  explicit SubscriptionEvents(detail::ObserverSlot<engine::SubscriptionObserver>& slot)
      : slot(slot) {}

  void on_subscription_event(const RoomEvent& event) override {
    if (auto observer = slot.get().lock()) {
      observer->on_subscription_event(event);
    }
  }

  detail::ObserverSlot<engine::SubscriptionObserver>& slot;
};
}  // namespace

RtcEngine::RtcEngine(RoomEngine::TransportFactory factory)
    : RtcEngine(std::unique_ptr<RoomEngine>(
          new RoomEngine(std::move(factory), std::make_shared<detail::SessionControl>()))) {}

RtcEngine::RtcEngine(std::unique_ptr<RoomEngine> room)
    : impl_(std::make_unique<Impl>(std::move(room))) {}

RtcEngine::~RtcEngine() = default;

RtcEngine::Impl::Impl(std::unique_ptr<RoomEngine> value)
    : room(std::move(value)), room_view(room.get()) {
  if (!room) {
    room = std::make_unique<RoomEngine>(RoomEngine::TransportFactory{});
    room_view = RoomView(room.get());
  }
  control = room->control();
  room_events = std::make_shared<RoomEvents>(control->async);
  room->set_internal_room_observer(room_events);
  publication_events = std::make_shared<PublicationEvents>(publication_observer);
  subscription_events = std::make_shared<SubscriptionEvents>(subscription_observer);
  room->set_publication_observer(publication_events);
  room->set_subscription_observer(subscription_events);
  graphs = std::make_unique<TrackGraphRegistry>();
  control->async->set_maintenance([this] {
    const auto result = execute([this] { reconcile_video_codec_policy(); }, true);
    if (result != ErrorCode::ok) {
      throw SDKError(ErrorDomain::lifecycle, result, "RTC failed graph reclamation failed");
    }
  });
  graphs->set_reap_handler([weak = std::weak_ptr<detail::AsyncControl>(control->async)] {
    if (auto queue = weak.lock()) {
      queue->request_maintenance();
    }
  });
  graphs->set_failure_handler([this](auto failure) {
    GraphFailure value{failure.id, failure.publication, failure.message, failure.failure};
    auto publisher = publication_observer.get();
    auto subscriber = subscription_observer.get();
    room->post_notification([value = std::move(value), publisher, subscriber] {
      if (value.publication) {
        if (auto observer = publisher.lock()) {
          observer->on_graph_failure(value);
        }
      } else if (auto observer = subscriber.lock()) {
        observer->on_graph_failure(value);
      }
    });
  });
}

RtcEngine::Impl::~Impl() {
  control->async->shutdown();
  control->async->set_maintenance({});
  // Applications must join all callers before destroying the facade.
  try {
    detail::OperationGuard guard(*control);
    room->set_event_handler_locked({});
    graphs->set_failure_handler({});
    graphs->set_reap_handler({});
    (void)clear_tracks();
    media.reset();
    media_resources.reset();
    graphs.reset();
    (void)room->leave_locked();
  } catch (...) {
  }
}

ErrorCode RtcEngine::Impl::clear_tracks() {
  media_publications.clear();
  return detach(graphs->take_all());
}

ErrorCode RtcEngine::Impl::detach(TrackGraphRegistry::RetiredGraphs retired) const {
  auto result = ErrorCode::ok;
  for (const auto& graph : retired.publications) {
    auto detached = graph->node().track()->close_locked();
    if (detached != ErrorCode::ok && result == ErrorCode::ok) {
      result = detached;
    }
  }
  for (const auto& graph : retired.subscriptions) {
    auto detached = graph->node().track()->close_locked();
    if (detached != ErrorCode::ok && result == ErrorCode::ok) {
      result = detached;
    }
  }
  return result;
}

Result<TrackId> RtcEngine::Impl::publish_graph(TrackDescriptor descriptor,
                                               PublishGraphFactory factory) {
  return detail::control_boundary([&]() -> Result<TrackId> {
    if (codec_policy && descriptor.kind == MediaKind::video &&
        ((descriptor.format.codec != Codec::h264 && descriptor.format.codec != Codec::h265) ||
         (!building_media && descriptor.format.codec != Codec::h264))) {
      return detail::failure_result<TrackId>(
          ErrorCode::unsupported,
          "Room codec policy requires managed H264/H265 media; encoded graphs must use H264");
    }
    if (!factory) {
      return detail::failure_result<TrackId>(ErrorCode::invalid_argument,
                                             "RTC engine requires a graph factory");
    }
    auto ready = room->publish_locked(descriptor);
    if (!ready) {
      return Result<TrackId>::failure(ready.failure_record());
    }
    auto endpoint = *ready;
    std::shared_ptr<nodes::RtcPublishNode> node;
    std::shared_ptr<PublishPipeline> graph;
    TrackGraphSpec prepared;
    try {
      node = std::make_shared<nodes::RtcPublishNode>("rtc.publish." + descriptor.id, endpoint);
      {
        detail::CallbackScope scope;
        prepared = factory(node);
      }
      graph = graphs->prepare(node, prepared);
      graphs->install(graph);
      if (!building_media) {
        // A caller may reuse the id of a failed managed publication for a
        // custom H264 graph. Never retain the previous source's restart recipe.
        media_publications.erase(descriptor.id);
      }
      return Result<TrackId>::success(descriptor.id);
    } catch (...) {
      auto error = normalized_failure(
          std::current_exception(), {"create_graph", descriptor.id, {}, endpoint->snapshot().epoch},
          ErrorDomain::media, ErrorCode::processing_failed);
      if (graph) {
        (void)graph->close_locked();
        graph->notify_failure(error);
      } else {
        detail::CallbackScope scope;
        if (prepared.quiesce) {
          prepared.quiesce();
        }
        if (prepared.on_failure) {
          prepared.on_failure(error);
        }
      }
      endpoint->report_failure(error);
      (void)endpoint->close_locked();
      return Result<TrackId>::failure(describe_failure(error));
    }
  });
}

Result<TrackId> RtcEngine::Impl::subscribe_graph(TrackDescriptor descriptor,
                                                 SubscribeGraphFactory factory) {
  return detail::control_boundary([&]() -> Result<TrackId> {
    if (!factory) {
      return detail::failure_result<TrackId>(ErrorCode::invalid_argument,
                                             "RTC engine requires a graph factory");
    }
    auto ready = room->subscribe_locked(descriptor);
    if (!ready) {
      return Result<TrackId>::failure(ready.failure_record());
    }
    auto endpoint = *ready;
    std::shared_ptr<nodes::RtcSubscribeNode> node;
    std::shared_ptr<SubscribePipeline> graph;
    TrackGraphSpec prepared;
    try {
      node = std::make_shared<nodes::RtcSubscribeNode>("rtc.subscribe." + descriptor.id, endpoint);
      {
        detail::CallbackScope scope;
        prepared = factory(node);
      }
      graph = graphs->prepare(node, prepared);
      graphs->install(graph);
      return Result<TrackId>::success(descriptor.id);
    } catch (...) {
      auto error = normalized_failure(
          std::current_exception(), {"create_graph", descriptor.id, {}, endpoint->snapshot().epoch},
          ErrorDomain::media, ErrorCode::processing_failed);
      if (graph) {
        (void)graph->close_locked();
        graph->notify_failure(error);
      } else {
        detail::CallbackScope scope;
        if (prepared.quiesce) {
          prepared.quiesce();
        }
        if (prepared.on_failure) {
          prepared.on_failure(error);
        }
      }
      endpoint->report_failure(error);
      (void)endpoint->close_locked();
      return Result<TrackId>::failure(describe_failure(error));
    }
  });
}

Result<TrackId> RtcEngine::Impl::publish(
    TrackDescriptor descriptor, std::shared_ptr<graphflow::core::GraphNode> feedback_sink) {
  return publish_graph(std::move(descriptor), [feedback_sink](auto sink) {
    TrackGraphSpec spec;
    if (feedback_sink) {
      spec.nodes.push_back(feedback_sink);
      spec.connections.push_back(
          TrackGraphSpec::connect<nodes::SenderFeedbackEvent>(sink, feedback_sink));
    }
    return spec;
  });
}

Result<TrackId> RtcEngine::Impl::subscribe(TrackDescriptor descriptor,
                                           std::shared_ptr<graphflow::core::GraphNode> consumer) {
  if (!consumer) {
    return detail::failure_result<TrackId>(ErrorCode::invalid_argument,
                                           "RTC engine requires a subscription consumer node");
  }
  return subscribe_graph(std::move(descriptor), [consumer](auto source) {
    TrackGraphSpec spec;
    spec.nodes.push_back(consumer);
    spec.connections.push_back(TrackGraphSpec::connect<nodes::EncodedFrameEvent>(source, consumer));
    return spec;
  });
}

bool RtcEngine::media_enabled() const {
  return impl_->has_media;
}

VideoCodecPolicySnapshot RtcEngine::video_codec_policy() const {
  std::lock_guard lock(impl_->policy_mutex);
  return impl_->policy_status;
}

ErrorCode RtcEngine::join(RoomOptions options) {
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "room.join",
        [this, options = std::move(options)]() mutable {
          return impl_->execute([state = impl_.get(), options = std::move(options)]() mutable {
            const auto connection = state->room->snapshot().state;
            if (connection != ConnectionState::disconnected &&
                connection != ConnectionState::closed && connection != ConnectionState::failed) {
              return ErrorCode::invalid_state;
            }
            if (connection == ConnectionState::failed || connection == ConnectionState::closed) {
              auto cleaned = state->clear_tracks();
              if (cleaned != ErrorCode::ok) {
                return cleaned;
              }
            }
            state->reset_video_codec_policy(options.video_codec_policy);
            options.video_codec_policy.reset();
            auto joined = state->room->join_locked(std::move(options));
            if (joined != ErrorCode::ok) {
              return joined;
            }
            if (state->codec_policy && !state->room->snapshot().session) {
              (void)state->room->leave_locked();
              throw SDKError(ErrorDomain::protocol, ErrorCode::unsupported,
                             "Room codec policy requires participant snapshots");
            }
            state->reconcile_video_codec_policy();
            std::shared_ptr<const FailureInfo> failure;
            {
              std::lock_guard lock(state->policy_mutex);
              failure = state->policy_status.failure;
            }
            if (failure) {
              (void)state->room->leave_locked();
              detail::control_failure = failure;
              return failure->code;
            }
            return ErrorCode::ok;
          });
        },
        impl_->room_observer.get(),
        [](auto& observer, auto result) { observer.on_join_completed(std::move(result)); });
  });
}

ErrorCode RtcEngine::leave() {
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "room.leave",
        [this]() mutable {
          return impl_->execute([state = impl_.get()] {
            auto graphs = state->clear_tracks();
            auto left = state->room->leave_locked();
            state->reset_video_codec_policy();
            return graphs == ErrorCode::ok ? left : graphs;
          });
        },
        impl_->room_observer.get(),
        [](auto& observer, auto result) { observer.on_leave_completed(std::move(result)); }, true,
        false);
  });
}

ErrorCode RtcEngine::close() {
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "room.close",
        [this]() mutable {
          return impl_->execute(
              [state = impl_.get()] {
                if (state->closed) {
                  return ErrorCode::ok;
                }
                state->control->closed = true;
                auto graphs = state->clear_tracks();
                auto left = state->room->leave_locked();
                state->reset_video_codec_policy();
                state->closed = graphs == ErrorCode::ok && left == ErrorCode::ok;
                return graphs == ErrorCode::ok ? left : graphs;
              },
              true);
        },
        impl_->room_observer.get(),
        [](auto& observer, auto result) { observer.on_close_completed(std::move(result)); }, true,
        true);
  });
}

Result<MediaPublication> RtcEngine::start_media(MediaPublishSpec spec,
                                                std::optional<MediaKind> expected) {
  return impl_->execute([&]() -> Result<MediaPublication> {
    if (expected && spec.track.kind != *expected) {
      return detail::failure_result<MediaPublication>(
          ErrorCode::invalid_argument, "Publication kind does not match the requested media API");
    }
    if (!impl_->media) {
      return detail::failure_result<MediaPublication>(ErrorCode::unsupported,
                                                      "RTC media module is not enabled");
    }
    impl_->reconcile_video_codec_policy();
    return impl_->start_media_locked(std::move(spec));
  });
}

ErrorCode RtcEngine::publish_media(MediaPublishSpec spec) {
  const auto track_id = spec.track.id;
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "publication.publish" + std::string(":") + track_id,
        [this, spec = std::move(spec)]() mutable { return start_media(std::move(spec)); },
        impl_->publication_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_media_publish_completed(track_id, std::move(result));
        });
  });
}

ErrorCode RtcEngine::publish_video(MediaPublishSpec spec) {
  const auto track_id = spec.track.id;
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "publication.publish" + std::string(":") + track_id,
        [this, spec = std::move(spec)]() mutable {
          return start_media(std::move(spec), MediaKind::video);
        },
        impl_->publication_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_media_publish_completed(track_id, std::move(result));
        });
  });
}

ErrorCode RtcEngine::publish_audio(MediaPublishSpec spec) {
  const auto track_id = spec.track.id;
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "publication.publish" + std::string(":") + track_id,
        [this, spec = std::move(spec)]() mutable {
          return start_media(std::move(spec), MediaKind::audio);
        },
        impl_->publication_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_media_publish_completed(track_id, std::move(result));
        });
  });
}

ErrorCode RtcEngine::subscribe_media(MediaSubscribeSpec spec) {
  const auto track_id = spec.track.id;
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "subscription.subscribe" + std::string(":") + track_id,
        [this, spec = std::move(spec)]() mutable {
          return impl_->execute([&]() -> Result<MediaSubscription> {
            if (!impl_->media) {
              return detail::failure_result<MediaSubscription>(ErrorCode::unsupported,
                                                               "RTC media module is not enabled");
            }
            auto& builder = *impl_->media;
            std::shared_ptr<MediaTrack> track;
            auto ready = impl_->subscribe_graph(spec.track, [&](auto node) {
              auto prepared = builder.build_subscribe(std::move(spec), std::move(node));
              track = std::move(prepared.track);
              return std::move(prepared.graph);
            });
            if (!ready) {
              return Result<MediaSubscription>::failure(ready.failure_record());
            }
            return Result<MediaSubscription>::success({*ready, std::move(track)});
          });
        },
        impl_->subscription_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_media_subscribe_completed(track_id, std::move(result));
        });
  });
}

ErrorCode RtcEngine::create_shared_input(TrackDescriptor format, MediaInput input) {
  const auto id = format.id;
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "publication.input:" + id,
        [this, format = std::move(format), input = std::move(input)]() mutable {
          return create_shared_input_impl(std::move(format), std::move(input));
        },
        impl_->publication_observer.get(),
        [id](auto& observer, auto result) {
          observer.on_shared_input_created(id, std::move(result));
        });
  });
}

Result<std::shared_ptr<SharedMediaInput>> RtcEngine::create_shared_input_impl(
    TrackDescriptor format, MediaInput input) {
  return impl_->execute([&] {
    if (!impl_->media) {
      return detail::failure_result<std::shared_ptr<SharedMediaInput>>(
          ErrorCode::unsupported, "RTC media module is not enabled");
    }
    return Result<std::shared_ptr<SharedMediaInput>>::success(
        impl_->media->create_shared_input(std::move(format), std::move(input)));
  });
}

ErrorCode RtcEngine::publish(TrackDescriptor descriptor) {
  const auto track_id = descriptor.id;
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "publication.publish" + std::string(":") + track_id,
        [this, descriptor = std::move(descriptor)]() mutable {
          return impl_->execute([&] { return impl_->publish(std::move(descriptor)); });
        },
        impl_->publication_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_publish_completed(track_id, std::move(result));
        });
  });
}

RtcGraphApi RtcEngine::graph() {
  return RtcGraphApi(*this);
}

ErrorCode RtcGraphApi::publish(TrackDescriptor descriptor,
                               std::shared_ptr<graphflow::core::GraphNode> sink) {
  const auto track_id = descriptor.id;
  auto* engine = engine_;
  return detail::control_boundary([&] {
    return engine->impl_->control->async->submit(
        "publication.publish:" + track_id,
        [engine, descriptor = std::move(descriptor), sink = std::move(sink)]() mutable {
          return engine->impl_->execute(
              [&] { return engine->impl_->publish(std::move(descriptor), std::move(sink)); });
        },
        engine->impl_->publication_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_publish_completed(track_id, std::move(result));
        });
  });
}

ErrorCode RtcGraphApi::subscribe(TrackDescriptor descriptor,
                                 std::shared_ptr<graphflow::core::GraphNode> consumer) {
  const auto track_id = descriptor.id;
  auto* engine = engine_;
  return detail::control_boundary([&] {
    return engine->impl_->control->async->submit(
        "subscription.subscribe:" + track_id,
        [engine, descriptor = std::move(descriptor), consumer = std::move(consumer)]() mutable {
          return engine->impl_->execute(
              [&] { return engine->impl_->subscribe(std::move(descriptor), std::move(consumer)); });
        },
        engine->impl_->subscription_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_subscribe_completed(track_id, std::move(result));
        });
  });
}

ErrorCode RtcGraphApi::publish_graph(TrackDescriptor descriptor, PublishGraphFactory factory) {
  const auto track_id = descriptor.id;
  auto* engine = engine_;
  return detail::control_boundary([&] {
    return engine->impl_->control->async->submit(
        "publication.publish:" + track_id,
        [engine, descriptor = std::move(descriptor), factory = std::move(factory)]() mutable {
          return engine->impl_->execute([&] {
            return engine->impl_->publish_graph(std::move(descriptor), std::move(factory));
          });
        },
        engine->impl_->publication_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_publish_completed(track_id, std::move(result));
        });
  });
}

ErrorCode RtcGraphApi::subscribe_graph(TrackDescriptor descriptor, SubscribeGraphFactory factory) {
  const auto track_id = descriptor.id;
  auto* engine = engine_;
  return detail::control_boundary([&] {
    return engine->impl_->control->async->submit(
        "subscription.subscribe:" + track_id,
        [engine, descriptor = std::move(descriptor), factory = std::move(factory)]() mutable {
          return engine->impl_->execute([&] {
            return engine->impl_->subscribe_graph(std::move(descriptor), std::move(factory));
          });
        },
        engine->impl_->subscription_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_subscribe_completed(track_id, std::move(result));
        });
  });
}

ErrorCode RtcEngine::unpublish(const TrackId& id) {
  const auto track_id = id;
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "publication.remove" + std::string(":") + track_id,
        [this, id]() mutable {
          return impl_->execute([state = impl_.get(), id] {
            state->media_publications.erase(id);
            auto retired = state->graphs->take(id, true);
            // Keep graph leases alive until the native remove operation completes.
            return state->room->unpublish_locked(id);
          });
        },
        impl_->publication_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_unpublish_completed(track_id, std::move(result));
        });
  });
}

ErrorCode RtcEngine::unsubscribe(const TrackId& id) {
  const auto track_id = id;
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "subscription.remove" + std::string(":") + track_id,
        [this, id]() mutable {
          return impl_->execute([state = impl_.get(), id] {
            auto retired = state->graphs->take(id, false);
            // Keep graph leases alive until the native remove operation completes.
            return state->room->unsubscribe_locked(id);
          });
        },
        impl_->subscription_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_unsubscribe_completed(track_id, std::move(result));
        });
  });
}

ErrorCode RtcEngine::set_muted(const TrackId& id, bool muted, ControlOptions options) {
  const auto track_id = id;
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "publication.mute" + std::string(":") + track_id,
        [this, id, muted, options]() mutable {
          return impl_->execute([state = impl_.get(), id, muted, options] {
            return state->room->set_muted_locked(id, muted, options);
          });
        },
        impl_->publication_observer.get(),
        [track_id, muted](auto& observer, auto result) {
          observer.on_muted_completed(track_id, muted, std::move(result));
        });
  });
}

ErrorCode RtcEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                        ControlOptions options) {
  const auto track_id = id;
  return detail::control_boundary([&] {
    return impl_->control->async->submit(
        "subscription.demand" + std::string(":") + track_id,
        [this, id, demand, options]() mutable {
          return impl_->execute([state = impl_.get(), id, demand, options] {
            return state->room->set_receive_demand_locked(id, demand, options);
          });
        },
        impl_->subscription_observer.get(),
        [track_id, demand](auto& observer, auto result) {
          observer.on_receive_demand_completed(track_id, demand, std::move(result));
        });
  });
}

WriteStatus RtcEngine::write(const TrackId& id, EncodedFrame frame) {
  return impl_->graphs->write(id, std::move(frame));
}

TrackSnapshot RtcEngine::publication(const TrackId& id) const {
  return impl_->graphs->publication(id);
}

SenderFeedback RtcEngine::feedback(const TrackId& id) const {
  return impl_->graphs->feedback(id);
}

void RtcEngine::set_room_observer(std::weak_ptr<RoomObserver> observer) {
  impl_->room_observer.set(observer);
  impl_->room->set_room_observer(std::move(observer));
}

void RtcEngine::set_publication_observer(std::weak_ptr<PublicationObserver> observer) {
  impl_->publication_observer.set(std::move(observer));
}

void RtcEngine::set_subscription_observer(std::weak_ptr<SubscriptionObserver> observer) {
  impl_->subscription_observer.set(std::move(observer));
}

const RoomView& RtcEngine::room() const {
  return impl_->room_view;
}

}  // namespace livekit::sdk::rtc::engine
