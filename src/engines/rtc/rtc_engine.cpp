#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "engines/rtc/rtc_engine_impl.hpp"
#include "backends/rtc/room_transport.hpp"
#include "livekit/error.hpp"

namespace livekit::sdk::rtc::engine {
RtcEngine::RtcEngine(RoomEngine::TransportFactory factory)
    : RtcEngine(std::unique_ptr<RoomEngine>(
          new RoomEngine(std::move(factory), std::make_shared<detail::SessionControl>()))) {}

RtcEngine::RtcEngine(std::unique_ptr<RoomEngine> room)
    : impl_(std::make_unique<Impl>(std::move(room))) {}

RtcEngine::~RtcEngine() = default;

RtcEngine::Impl::Impl(std::unique_ptr<RoomEngine> value)
    : room(std::move(value)), room_view(room.get()) {
  if (!room) {
    throw std::invalid_argument("RTC engine requires a room engine");
  }
  control = room->control();
  graphs = std::make_unique<TrackGraphRegistry>();
}

RtcEngine::Impl::~Impl() {
  // Applications must join all callers before destroying the facade.
  try {
    detail::OperationGuard guard(*control);
    if (callback_active) {
      *callback_active = false;
    }
    room->set_event_handler_locked({});
    graphs->set_failure_handler({});
    (void)clear_tracks();
    media.reset();
    media_resources.reset();
    graphs.reset();
    (void)room->leave_locked();
  } catch (...) {
  }
}

void RtcEngine::Impl::require_open() const {
  if (control->closed) {
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed, "RTC engine closed");
  }
}

MediaGraphBuilder& RtcEngine::Impl::require_media() const {
  require_open();
  if (!media) {
    throw SDKError(ErrorDomain::media, ErrorCode::unsupported, "RTC media module is not enabled");
  }
  return *media;
}

Result<void> RtcEngine::Impl::clear_tracks() {
  return detach(graphs->take_all());
}

Result<void> RtcEngine::Impl::detach(TrackGraphRegistry::RetiredGraphs retired) const {
  auto result = Result<void>::success();
  for (const auto& graph : retired.publications) {
    auto detached = graph->node().track()->close_locked();
    if (!detached && result) {
      result = std::move(detached);
    }
  }
  for (const auto& graph : retired.subscriptions) {
    auto detached = graph->node().track()->close_locked();
    if (!detached && result) {
      result = std::move(detached);
    }
  }
  return result;
}

Result<TrackId> RtcEngine::Impl::publish_graph(TrackDescriptor descriptor,
                                               PublishGraphFactory factory) {
  return detail::control_boundary([&]() -> Result<TrackId> {
    if (!factory) {
      throw std::invalid_argument("RTC engine requires a graph factory");
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
      throw std::invalid_argument("RTC engine requires a graph factory");
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
    throw std::invalid_argument("RTC engine requires a subscription consumer node");
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

Result<void> RtcEngine::join(RoomOptions options) {
  return impl_->execute([state = impl_.get(), options = std::move(options)]() mutable {
    state->require_open();
    const auto connection = state->room->snapshot().state;
    if (connection == ConnectionState::failed || connection == ConnectionState::closed) {
      auto cleaned = state->clear_tracks();
      if (!cleaned) {
        return cleaned;
      }
    }
    return state->room->join_locked(std::move(options));
  });
}

Result<void> RtcEngine::leave() {
  return impl_->execute([state = impl_.get()] {
    state->require_open();
    auto graphs = state->clear_tracks();
    auto left = state->room->leave_locked();
    return graphs ? left : graphs;
  });
}

Result<void> RtcEngine::close() {
  return impl_->execute([state = impl_.get()] {
    if (state->closed) {
      return Result<void>::success();
    }
    state->control->closed = true;
    if (state->callback_active) {
      *state->callback_active = false;
    }
    state->room->set_event_handler_locked({});
    auto graphs = state->clear_tracks();
    auto left = state->room->leave_locked();
    state->closed = bool(graphs) && bool(left);
    return graphs ? left : graphs;
  });
}

Result<MediaPublication> RtcEngine::start_media(MediaPublishSpec spec,
                                                std::optional<MediaKind> expected) {
  return impl_->execute([&]() -> Result<MediaPublication> {
    if (expected && spec.track.kind != *expected) {
      throw std::invalid_argument("Publication kind does not match the requested media API");
    }
    auto& builder = impl_->require_media();
    std::shared_ptr<MediaTrack> track;
    auto ready = impl_->publish_graph(spec.track, [&](auto node) {
      auto prepared = builder.build_publish(std::move(spec), std::move(node));
      track = std::move(prepared.track);
      return std::move(prepared.graph);
    });
    if (!ready) {
      return Result<MediaPublication>::failure(ready.failure_record());
    }
    return Result<MediaPublication>::success({*ready, std::move(track)});
  });
}

Result<MediaPublication> RtcEngine::publish_media(MediaPublishSpec spec) {
  return start_media(std::move(spec));
}

Result<MediaPublication> RtcEngine::publish_video(MediaPublishSpec spec) {
  return start_media(std::move(spec), MediaKind::video);
}

Result<MediaPublication> RtcEngine::publish_audio(MediaPublishSpec spec) {
  return start_media(std::move(spec), MediaKind::audio);
}

Result<MediaSubscription> RtcEngine::subscribe_media(MediaSubscribeSpec spec) {
  return impl_->execute([&]() -> Result<MediaSubscription> {
    auto& builder = impl_->require_media();
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
}

Result<std::shared_ptr<SharedMediaInput>> RtcEngine::create_shared_input(TrackDescriptor format,
                                                                         MediaInput input) {
  return impl_->execute(
      [state = impl_.get(), format = std::move(format), input = std::move(input)]() mutable {
        return Result<std::shared_ptr<SharedMediaInput>>::success(
            state->require_media().create_shared_input(std::move(format), std::move(input)));
      });
}

Result<TrackId> RtcEngine::publish(TrackDescriptor descriptor) {
  return impl_->execute([state = impl_.get(), descriptor = std::move(descriptor)]() mutable {
    state->require_open();
    return state->publish(std::move(descriptor));
  });
}

RtcGraphApi RtcEngine::graph() {
  return RtcGraphApi(*this);
}

Result<TrackId> RtcGraphApi::publish(TrackDescriptor descriptor,
                                     std::shared_ptr<graphflow::core::GraphNode> sink) {
  return engine_->impl_->execute([state = engine_->impl_.get(), descriptor = std::move(descriptor),
                                  sink = std::move(sink)]() mutable {
    state->require_open();
    return state->publish(std::move(descriptor), std::move(sink));
  });
}

Result<TrackId> RtcGraphApi::subscribe(TrackDescriptor descriptor,
                                       std::shared_ptr<graphflow::core::GraphNode> consumer) {
  return engine_->impl_->execute([state = engine_->impl_.get(), descriptor = std::move(descriptor),
                                  consumer = std::move(consumer)]() mutable {
    state->require_open();
    return state->subscribe(std::move(descriptor), std::move(consumer));
  });
}

Result<TrackId> RtcGraphApi::publish_graph(TrackDescriptor descriptor,
                                           PublishGraphFactory factory) {
  return engine_->impl_->execute([state = engine_->impl_.get(), descriptor = std::move(descriptor),
                                  factory = std::move(factory)]() mutable {
    state->require_open();
    return state->publish_graph(std::move(descriptor), std::move(factory));
  });
}

Result<TrackId> RtcGraphApi::subscribe_graph(TrackDescriptor descriptor,
                                             SubscribeGraphFactory factory) {
  return engine_->impl_->execute([state = engine_->impl_.get(), descriptor = std::move(descriptor),
                                  factory = std::move(factory)]() mutable {
    state->require_open();
    return state->subscribe_graph(std::move(descriptor), std::move(factory));
  });
}

Result<void> RtcEngine::unpublish(const TrackId& id) {
  return impl_->execute([state = impl_.get(), id] {
    state->require_open();
    auto retired = state->graphs->take(id, true);
    // Keep graph leases alive until the native remove operation completes.
    return state->room->unpublish_locked(id);
  });
}

Result<void> RtcEngine::unsubscribe(const TrackId& id) {
  return impl_->execute([state = impl_.get(), id] {
    state->require_open();
    auto retired = state->graphs->take(id, false);
    // Keep graph leases alive until the native remove operation completes.
    return state->room->unsubscribe_locked(id);
  });
}

Result<void> RtcEngine::set_muted(const TrackId& id, bool muted, ControlOptions options) {
  return impl_->execute([state = impl_.get(), id, muted, options] {
    state->require_open();
    return state->room->set_muted_locked(id, muted, options);
  });
}

Result<void> RtcEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                           ControlOptions options) {
  return impl_->execute([state = impl_.get(), id, demand, options] {
    state->require_open();
    return state->room->set_receive_demand_locked(id, demand, options);
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

Result<void> RtcEngine::set_callbacks(Callbacks callbacks) {
  return impl_->execute([&] {
    if (impl_->control->closed && (callbacks.on_event || callbacks.on_failure)) {
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed, "RTC engine closed");
    }
    auto active = std::make_shared<std::atomic<bool>>(true);
    if (impl_->callback_active) {
      *impl_->callback_active = false;
    }
    impl_->callback_active = active;
    impl_->graphs->set_failure_handler(
        [state = impl_.get(), active, handler = std::move(callbacks.on_failure)](auto failure) {
          if (handler) {
            state->room->post_notification([active, handler, failure = std::move(failure)] {
              if (*active) {
                try {
                  handler(GraphFailure{failure.id, failure.publication, failure.message,
                                       failure.failure});
                } catch (...) {
                  *active = false;
                  throw;
                }
              }
            });
          }
        });
    // Control reentry is rejected before the operation lock.
    std::function<void(const RoomEvent&)> event;
    if (callbacks.on_event) {
      event = [active, handler = std::move(callbacks.on_event)](const RoomEvent& value) {
        if (*active) {
          try {
            handler(value);
          } catch (...) {
            *active = false;
            throw;
          }
        }
      };
    }
    impl_->room->set_event_handler_locked(std::move(event));
  });
}

const RoomView& RtcEngine::room() const {
  return impl_->room_view;
}

}  // namespace livekit::sdk::rtc::engine
