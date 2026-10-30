#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "engines/rtc/rtc_engine_control.hpp"
#include "backends/rtc/room_transport.hpp"
#include "livekit/error.hpp"

namespace livekit::sdk::rtc::engine {
RtcEngine::RtcEngine(TransportFactory factory)
    : runtime(std::make_unique<detail::RtcExecutionContext>(std::move(factory))),
      lifecycle(runtime->lifecycle()),
      room_view(this) {
  control = runtime->control();
  runtime->set_event_handler([this](const RoomEvent& event) {
    if (!event.publication) {
      if (event.kind == RoomEventKind::session_changed || event.kind == RoomEventKind::connected) {
        control->async->request_maintenance();
      }
      if (auto observer = room_observer.get().lock()) {
        observer->on_room_event(event);
      }
    } else if (*event.publication) {
      if (auto observer = publication_observer.get().lock()) {
        observer->on_publication_event(event);
      }
    } else if (auto observer = subscription_observer.get().lock()) {
      observer->on_subscription_event(event);
    }
  });
  graphs = std::make_unique<TrackGraphRegistry>();
  control->async->set_async_maintenance([this] {
    return lifecycle.submit(
        [this] { return execute([this] { reconcile_video_codec_policy(); }, true); });
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
    runtime->post_notification([value = std::move(value), publisher, subscriber] {
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

RtcEngine::~RtcEngine() {
  control->async->shutdown();
  lifecycle.shutdown();
  control->async->set_maintenance({});
  // Applications must join all callers before destroying the facade.
  try {
    detail::OperationGuard guard(*control);
    runtime->set_event_handler({});
    graphs->set_failure_handler({});
    graphs->set_reap_handler({});
    (void)clear_tracks();
    media.reset();
    media_resources.reset();
    graphs.reset();
    (void)runtime
        ->invoke<void>([&](RoomEngine& protocol, auto done) { protocol.leave(std::move(done)); })
        .get();
  } catch (...) {
  }
}

ErrorCode RtcEngine::clear_tracks() {
  media_publications.clear();
  return detach(graphs->take_all());
}

ErrorCode RtcEngine::detach(TrackGraphRegistry::RetiredGraphs retired) const {
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

Result<TrackId> RtcEngine::publish_graph(TrackDescriptor descriptor, PublishGraphFactory factory) {
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
    auto ready =
        runtime
            ->invoke<std::shared_ptr<PublishedTrack>>([&](RoomEngine& protocol, auto done) {
              protocol.publish(descriptor, std::move(done));
            })
            .get();
    if (!ready) {
      return Result<TrackId>::failure(ready.failure_record());
    }
    return finish_publish_graph(std::move(descriptor), std::move(factory), *ready);
  });
}

Result<TrackId> RtcEngine::finish_publish_graph(TrackDescriptor descriptor,
                                                PublishGraphFactory factory,
                                                std::shared_ptr<PublishedTrack> endpoint) {
  return detail::control_boundary([&]() -> Result<TrackId> {
    if (control->interrupted) {
      (void)endpoint->close_locked();
      return detail::failure_result<TrackId>(ErrorCode::cancelled, "RTC graph creation cancelled");
    }
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

Result<TrackId> RtcEngine::subscribe_graph(TrackDescriptor descriptor,
                                           SubscribeGraphFactory factory) {
  return detail::control_boundary([&]() -> Result<TrackId> {
    if (!factory) {
      return detail::failure_result<TrackId>(ErrorCode::invalid_argument,
                                             "RTC engine requires a graph factory");
    }
    auto ready =
        runtime
            ->invoke<std::shared_ptr<SubscribedTrack>>([&](RoomEngine& protocol, auto done) {
              protocol.subscribe(descriptor, std::move(done));
            })
            .get();
    if (!ready) {
      return Result<TrackId>::failure(ready.failure_record());
    }
    return finish_subscribe_graph(std::move(descriptor), std::move(factory), *ready);
  });
}

Result<TrackId> RtcEngine::finish_subscribe_graph(TrackDescriptor descriptor,
                                                  SubscribeGraphFactory factory,
                                                  std::shared_ptr<SubscribedTrack> endpoint) {
  return detail::control_boundary([&]() -> Result<TrackId> {
    if (control->interrupted) {
      (void)endpoint->close_locked();
      return detail::failure_result<TrackId>(ErrorCode::cancelled, "RTC graph creation cancelled");
    }
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

Result<TrackId> RtcEngine::publish_node(TrackDescriptor descriptor,
                                        std::shared_ptr<graphflow::core::GraphNode> feedback_sink) {
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

Result<TrackId> RtcEngine::subscribe_node(TrackDescriptor descriptor,
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

namespace {
template <class T>
AsyncResult<T> settled(T value) {
  CompletionSource<T> source;
  auto result = source.get_result();
  source.set_value(std::move(value));
  return result;
}
}  // namespace

AsyncResult<Result<TrackId>> RtcEngine::publish_graph_async(TrackDescriptor descriptor,
                                                            PublishGraphFactory factory,
                                                            bool managed) {
  if (!factory) {
    return settled(detail::failure_result<TrackId>(ErrorCode::invalid_argument,
                                                   "RTC engine requires a graph factory"));
  }
  if (codec_policy && descriptor.kind == MediaKind::video &&
      ((descriptor.format.codec != Codec::h264 && descriptor.format.codec != Codec::h265) ||
       (!managed && descriptor.format.codec != Codec::h264))) {
    return settled(detail::failure_result<TrackId>(
        ErrorCode::unsupported,
        "Room codec policy requires managed H264/H265 media; encoded graphs must use H264"));
  }
  auto pending = runtime->invoke<std::shared_ptr<PublishedTrack>>(
      [&](RoomEngine& protocol, auto done) { protocol.publish(descriptor, std::move(done)); });
  return std::move(pending).then(
      lifecycle.dispatcher(),
      [this, descriptor = std::move(descriptor), factory = std::move(factory),
       managed](AsyncResult<Result<std::shared_ptr<PublishedTrack>>> ready) mutable {
        auto result = ready.get();
        if (!result) {
          return Result<TrackId>::failure(result.failure_record());
        }
        return execute([&] {
          auto previous = std::exchange(building_media, managed);
          auto completed = finish_publish_graph(std::move(descriptor), std::move(factory), *result);
          building_media = previous;
          return completed;
        });
      });
}

AsyncResult<Result<TrackId>> RtcEngine::subscribe_graph_async(TrackDescriptor descriptor,
                                                              SubscribeGraphFactory factory) {
  if (!factory) {
    return settled(detail::failure_result<TrackId>(ErrorCode::invalid_argument,
                                                   "RTC engine requires a graph factory"));
  }
  auto pending = runtime->invoke<std::shared_ptr<SubscribedTrack>>(
      [&](RoomEngine& protocol, auto done) { protocol.subscribe(descriptor, std::move(done)); });
  return std::move(pending).then(
      lifecycle.dispatcher(),
      [this, descriptor = std::move(descriptor), factory = std::move(factory)](
          AsyncResult<Result<std::shared_ptr<SubscribedTrack>>> ready) mutable {
        auto result = ready.get();
        if (!result) {
          return Result<TrackId>::failure(result.failure_record());
        }
        return execute([&] {
          return finish_subscribe_graph(std::move(descriptor), std::move(factory), *result);
        });
      });
}

AsyncResult<Result<MediaPublication>> RtcEngine::start_media_async(
    MediaPublishSpec spec, std::optional<MediaKind> expected) {
  auto validated = execute([&]() -> Result<void> {
    if (expected && spec.track.kind != *expected) {
      return detail::failure_result<void>(
          ErrorCode::invalid_argument, "Publication kind does not match the requested media API");
    }
    if (!media) {
      return detail::failure_result<void>(ErrorCode::unsupported,
                                          "RTC media module is not enabled");
    }
    reconcile_video_codec_policy();
    if (codec_policy) {
      const auto snapshot = runtime->snapshot();
      if (snapshot.state != ConnectionState::connected || !snapshot.session) {
        return detail::failure_result<void>(ErrorCode::invalid_state,
                                            "Codec policy requires a current roster");
      }
      bool h264;
      {
        std::lock_guard lock(policy_mutex);
        h264 = policy_status.h264_latched;
      }
      if (h264) {
        use_h264(spec);
      }
    }
    return Result<void>::success();
  });
  if (!validated) {
    return settled(Result<MediaPublication>::failure(validated.failure_record()));
  }
  auto recipe = spec;
  auto track = std::make_shared<std::shared_ptr<MediaTrack>>();
  auto descriptor = spec.track;
  auto pending = publish_graph_async(
      std::move(descriptor),
      [this, spec = std::move(spec), track](auto node) mutable {
        auto prepared = media->build_publish(std::move(spec), std::move(node));
        *track = std::move(prepared.track);
        return std::move(prepared.graph);
      },
      true);
  return std::move(pending).then(
      lifecycle.dispatcher(),
      [this, recipe = std::move(recipe), track](AsyncResult<Result<TrackId>> ready) mutable {
        auto result = ready.get();
        if (!result) {
          return Result<MediaPublication>::failure(result.failure_record());
        }
        if (codec_policy && recipe.track.kind == MediaKind::video) {
          media_publications.insert_or_assign(*result, std::move(recipe));
        }
        return Result<MediaPublication>::success({*result, std::move(*track)});
      });
}

bool RtcEngine::media_enabled() const {
  return this->has_media;
}

VideoCodecPolicySnapshot RtcEngine::video_codec_policy() const {
  std::lock_guard lock(this->policy_mutex);
  return this->policy_status;
}

ErrorCode RtcEngine::join(RoomOptions options, Completion<void> completion) {
  return this->submit(
      "room.join",
      [state = this, options = std::move(options)]() mutable {
        auto prepared = state->execute([&] {
          const auto connection = state->runtime->snapshot().state;
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
          return ErrorCode::ok;
        });
        if (prepared != ErrorCode::ok) {
          throw SDKError(detail::operation_result(prepared, "join.prepare").failure_record());
        }
        auto pending = state->runtime->invoke<void>([&](RoomEngine& protocol, auto done) {
          protocol.join(std::move(options), std::move(done));
        });
        return std::move(pending).then(
            state->lifecycle.dispatcher(), [state](AsyncResult<ErrorCode> ready) {
              auto joined = ready.get();
              if (joined != ErrorCode::ok) {
                return joined;
              }
              auto result = state->execute([&] {
                if (state->codec_policy && !state->runtime->snapshot().session) {
                  (void)state->runtime
                      ->invoke<void>(
                          [&](RoomEngine& protocol, auto done) { protocol.leave(std::move(done)); })
                      .get();
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
                  (void)state->runtime
                      ->invoke<void>(
                          [&](RoomEngine& protocol, auto done) { protocol.leave(std::move(done)); })
                      .get();
                  throw SDKError(failure);
                }
                return ErrorCode::ok;
              });
              if (result != ErrorCode::ok) {
                throw SDKError(detail::operation_result(result, "join.finalize").failure_record());
              }
              return result;
            });
      },
      std::move(completion));
}

ErrorCode RtcEngine::leave(Completion<void> completion) {
  return this->submit(
      "room.leave",
      [this]() mutable {
        return this->execute([state = this] {
          auto graphs = state->clear_tracks();
          auto left = state->runtime
                          ->invoke<void>([&](RoomEngine& protocol, auto done) {
                            protocol.leave(std::move(done));
                          })
                          .get();
          state->reset_video_codec_policy();
          return graphs == ErrorCode::ok ? left : graphs;
        });
      },
      std::move(completion), true, false);
}

ErrorCode RtcEngine::close(Completion<void> completion) {
  return this->submit(
      "room.close",
      [this]() mutable {
        return this->execute(
            [state = this] {
              if (state->closed) {
                return ErrorCode::ok;
              }
              state->control->closed = true;
              auto graphs = state->clear_tracks();
              auto left = state->runtime
                              ->invoke<void>([&](RoomEngine& protocol, auto done) {
                                protocol.leave(std::move(done));
                              })
                              .get();
              state->reset_video_codec_policy();
              state->closed = graphs == ErrorCode::ok && left == ErrorCode::ok;
              return graphs == ErrorCode::ok ? left : graphs;
            },
            true);
      },
      std::move(completion), true, true);
}

ErrorCode RtcEngine::publish_media(MediaPublishSpec spec, Completion<MediaPublication> completion) {
  const auto track_id = spec.track.id;
  return this->submit(
      "publication.publish" + std::string(":") + track_id,
      [this, spec = std::move(spec)]() mutable { return this->start_media_async(std::move(spec)); },
      std::move(completion));
}

ErrorCode RtcEngine::publish_video(MediaPublishSpec spec, Completion<MediaPublication> completion) {
  const auto track_id = spec.track.id;
  return this->submit(
      "publication.publish" + std::string(":") + track_id,
      [this, spec = std::move(spec)]() mutable {
        return this->start_media_async(std::move(spec), MediaKind::video);
      },
      std::move(completion));
}

ErrorCode RtcEngine::publish_audio(MediaPublishSpec spec, Completion<MediaPublication> completion) {
  const auto track_id = spec.track.id;
  return this->submit(
      "publication.publish" + std::string(":") + track_id,
      [this, spec = std::move(spec)]() mutable {
        return this->start_media_async(std::move(spec), MediaKind::audio);
      },
      std::move(completion));
}

ErrorCode RtcEngine::subscribe_media(MediaSubscribeSpec spec,
                                     Completion<MediaSubscription> completion) {
  const auto id = spec.track.id;
  return this->submit(
      "subscription.subscribe:" + id,
      [this, spec = std::move(spec)]() mutable {
        if (!this->has_media.load()) {
          return settled(detail::failure_result<MediaSubscription>(
              ErrorCode::unsupported, "RTC media module is not enabled"));
        }
        auto track = std::make_shared<std::shared_ptr<MediaTrack>>();
        auto descriptor = spec.track;
        auto pending = this->subscribe_graph_async(
            std::move(descriptor), [this, spec = std::move(spec), track](auto node) mutable {
              auto prepared = this->media->build_subscribe(std::move(spec), std::move(node));
              *track = std::move(prepared.track);
              return std::move(prepared.graph);
            });
        return std::move(pending).then(
            this->lifecycle.dispatcher(), [track](AsyncResult<Result<TrackId>> ready) {
              auto result = ready.get();
              if (!result) {
                return Result<MediaSubscription>::failure(result.failure_record());
              }
              return Result<MediaSubscription>::success({*result, std::move(*track)});
            });
      },
      std::move(completion));
}

ErrorCode RtcEngine::create_shared_input(TrackDescriptor format, MediaInput input,
                                         Completion<std::shared_ptr<SharedMediaInput>> completion) {
  const auto id = format.id;
  return this->submit(
      "publication.input:" + id,

      [this, format = std::move(format), input = std::move(input)]() mutable {
        return create_shared_input_impl(std::move(format), std::move(input));
      },
      std::move(completion));
}

Result<std::shared_ptr<SharedMediaInput>> RtcEngine::create_shared_input_impl(
    TrackDescriptor format, MediaInput input) {
  return this->execute([&] {
    if (!this->media) {
      return detail::failure_result<std::shared_ptr<SharedMediaInput>>(
          ErrorCode::unsupported, "RTC media module is not enabled");
    }
    return Result<std::shared_ptr<SharedMediaInput>>::success(
        this->media->create_shared_input(std::move(format), std::move(input)));
  });
}

ErrorCode RtcEngine::publish(TrackDescriptor descriptor, Completion<TrackId> completion) {
  const auto track_id = descriptor.id;
  return this->submit(
      "publication.publish" + std::string(":") + track_id,
      [this, descriptor = std::move(descriptor)]() mutable {
        return this->publish_graph_async(std::move(descriptor),
                                         [](auto) { return TrackGraphSpec{}; });
      },
      std::move(completion));
}

RtcGraphApi RtcEngine::graph() {
  return RtcGraphApi(*this);
}

ErrorCode RtcGraphApi::publish(TrackDescriptor descriptor,
                               std::shared_ptr<graphflow::core::GraphNode> sink,
                               Completion<TrackId> completion) {
  const auto track_id = descriptor.id;
  auto* engine = engine_;
  return engine->submit(
      "publication.publish:" + track_id,

      [engine, descriptor = std::move(descriptor), sink = std::move(sink)]() mutable {
        return engine->publish_graph_async(std::move(descriptor), [sink](auto node) {
          TrackGraphSpec spec;
          if (sink) {
            spec.nodes.push_back(sink);
            spec.connections.push_back(
                TrackGraphSpec::connect<nodes::SenderFeedbackEvent>(node, sink));
          }
          return spec;
        });
      },
      std::move(completion));
}

ErrorCode RtcGraphApi::subscribe(TrackDescriptor descriptor,
                                 std::shared_ptr<graphflow::core::GraphNode> consumer,
                                 Completion<TrackId> completion) {
  const auto track_id = descriptor.id;
  auto* engine = engine_;
  return engine->submit(
      "subscription.subscribe:" + track_id,
      [engine, descriptor = std::move(descriptor), consumer = std::move(consumer)]() mutable {
        if (!consumer) {
          return settled(detail::failure_result<TrackId>(ErrorCode::invalid_argument,
                                                         "RTC subscription requires a consumer"));
        }
        return engine->subscribe_graph_async(std::move(descriptor), [consumer](auto node) {
          TrackGraphSpec spec;
          spec.nodes.push_back(consumer);
          spec.connections.push_back(
              TrackGraphSpec::connect<nodes::EncodedFrameEvent>(node, consumer));
          return spec;
        });
      },
      std::move(completion));
}

ErrorCode RtcGraphApi::publish_graph(TrackDescriptor descriptor, PublishGraphFactory factory,
                                     Completion<TrackId> completion) {
  const auto track_id = descriptor.id;
  auto* engine = engine_;
  return engine->submit(
      "publication.publish:" + track_id,
      [engine, descriptor = std::move(descriptor), factory = std::move(factory)]() mutable {
        return engine->publish_graph_async(std::move(descriptor), std::move(factory));
      },
      std::move(completion));
}

ErrorCode RtcGraphApi::subscribe_graph(TrackDescriptor descriptor, SubscribeGraphFactory factory,
                                       Completion<TrackId> completion) {
  const auto track_id = descriptor.id;
  auto* engine = engine_;
  return engine->submit(
      "subscription.subscribe:" + track_id,
      [engine, descriptor = std::move(descriptor), factory = std::move(factory)]() mutable {
        return engine->subscribe_graph_async(std::move(descriptor), std::move(factory));
      },
      std::move(completion));
}

ErrorCode RtcEngine::unpublish(const TrackId& id, Completion<void> completion) {
  const auto track_id = id;
  return this->submit(
      "publication.remove" + std::string(":") + track_id,
      [this, id]() mutable {
        this->media_publications.erase(id);
        auto retired =
            std::make_shared<TrackGraphRegistry::RetiredGraphs>(this->graphs->take(id, true));
        auto pending = this->runtime->invoke<void>(
            [&](RoomEngine& protocol, auto done) { protocol.unpublish(id, std::move(done)); });
        return std::move(pending).then(
            this->lifecycle.dispatcher(),
            [retired](AsyncResult<ErrorCode> ready) { return ready.get(); });
      },
      std::move(completion));
}

ErrorCode RtcEngine::unsubscribe(const TrackId& id, Completion<void> completion) {
  const auto track_id = id;
  return this->submit(
      "subscription.remove" + std::string(":") + track_id,
      [this, id]() mutable {
        auto retired =
            std::make_shared<TrackGraphRegistry::RetiredGraphs>(this->graphs->take(id, false));
        auto pending = this->runtime->invoke<void>(
            [&](RoomEngine& protocol, auto done) { protocol.unsubscribe(id, std::move(done)); });
        return std::move(pending).then(
            this->lifecycle.dispatcher(),
            [retired](AsyncResult<ErrorCode> ready) { return ready.get(); });
      },
      std::move(completion));
}

ErrorCode RtcEngine::set_muted(const TrackId& id, bool muted, Completion<void> completion,
                               ControlOptions options) {
  const auto track_id = id;
  return this->submit(
      "publication.mute" + std::string(":") + track_id,
      [this, id, muted, options]() mutable {
        return this->runtime->invoke<void>([&](RoomEngine& protocol, auto done) {
          protocol.set_muted(id, muted, options, std::move(done));
        });
      },
      std::move(completion));
}

ErrorCode RtcEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                        Completion<void> completion, ControlOptions options) {
  const auto track_id = id;
  return this->submit(
      "subscription.demand" + std::string(":") + track_id,
      [this, id, demand, options]() mutable {
        return this->runtime->invoke<void>([&](RoomEngine& protocol, auto done) {
          protocol.set_receive_demand(id, demand, options, std::move(done));
        });
      },
      std::move(completion));
}

WriteStatus RtcEngine::write(const TrackId& id, EncodedFrame frame) {
  return this->graphs->write(id, std::move(frame));
}

TrackSnapshot RtcEngine::publication(const TrackId& id) const {
  return this->graphs->publication(id);
}

SenderFeedback RtcEngine::feedback(const TrackId& id) const {
  return this->graphs->feedback(id);
}

void RtcEngine::set_room_observer(std::weak_ptr<RoomObserver> observer) {
  this->room_observer.set(observer);
}

void RtcEngine::set_publication_observer(std::weak_ptr<PublicationObserver> observer) {
  this->publication_observer.set(std::move(observer));
}

void RtcEngine::set_subscription_observer(std::weak_ptr<SubscriptionObserver> observer) {
  this->subscription_observer.set(std::move(observer));
}

const RoomView& RtcEngine::room() const {
  return this->room_view;
}

}  // namespace livekit::sdk::rtc::engine

namespace livekit::sdk::rtc {
RoomSnapshot RoomView::snapshot() const {
  return engine_->runtime->snapshot();
}

Result<std::shared_ptr<RoomServices>> RoomView::services() const {
  return engine_->runtime->services();
}

ErrorCode RoomView::update_local_participant(ParticipantUpdate update, Completion<void> completion,
                                             ControlOptions options) const {
  auto* state = engine_;
  return detail::control_boundary([&] {
    return state->control->async->submit(
        "room.participant",
        [state, update = std::move(update), options]() mutable {
          return state->runtime->invoke<void>([&](RoomEngine& protocol, auto done) {
            protocol.update_local_participant(std::move(update), options, std::move(done));
          });
        },
        std::move(completion));
  });
}

ErrorCode RoomView::set_subscription_permissions(SubscriptionPermissions policy,
                                                 Completion<void> completion,
                                                 ControlOptions options) const {
  auto* state = engine_;
  return detail::control_boundary([&] {
    return state->control->async->submit(
        "room.permissions",
        [state, policy = std::move(policy), options]() mutable {
          return state->runtime->invoke<void>([&](RoomEngine& protocol, auto done) {
            protocol.set_subscription_permissions(std::move(policy), options, std::move(done));
          });
        },
        std::move(completion));
  });
}
}  // namespace livekit::sdk::rtc
