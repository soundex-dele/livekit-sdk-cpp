#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "engines/rtc/rtc_engine_impl.hpp"
#include "backends/rtc/room_transport.hpp"
#include "livekit/error.hpp"
#include "utoolkit/task_queue/task_queue.h"

namespace livekit::sdk::rtc::engine {
RtcEngine::RtcEngine(RoomEngine::TransportFactory factory)
    : RtcEngine(std::make_unique<RoomEngine>(std::move(factory))) {}

RtcEngine::RtcEngine(std::unique_ptr<RoomEngine> room)
    : impl_(std::make_unique<Impl>(std::move(room))) {}

RtcEngine::~RtcEngine() = default;

RtcEngine::Impl::Impl(std::unique_ptr<RoomEngine> value)
    : room(std::move(value)), room_view(room.get()) {
  if (!room) {
    throw std::invalid_argument("RTC engine requires a room engine");
  }
  control = vi::TaskQueue::create("rtc.engine.control");
  completion_dispatch.bind(*control);
  call([this] { pipelines = std::make_unique<RtcPipelineEngine>(*room, completion_dispatch); });
}

RtcEngine::Impl::~Impl() {
  // Unregister waits for an in-flight notifier, then the queue fence drains
  // already posted notifications before destroying their referenced modules.
  call([this] {
    if (callback_active) {
      *callback_active = false;
    }
    pipelines->set_failure_handler({});
  });
  room->set_event_handler({});
  try {
    call([this] {
      stopping = true;
      clear_tracks();
      media.reset();
      pipelines.reset();
    });
  } catch (...) {
  }
  completion_dispatch.close();
  control.reset();
  try {
    room->leave().get();
  } catch (...) {
  }
}

bool RtcEngine::Impl::control_is_current() const {
  return control->isCurrent();
}

void RtcEngine::Impl::dispatch(std::function<void()> task) const {
  if (queued_commands.fetch_add(1) >= 1024) {
    --queued_commands;
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                   "RTC control task queue exhausted");
  }
  try {
    control->postTask([this, task = std::move(task)] {
      --queued_commands;
      task();  // Packaged task or AsyncResult continuation captures exceptions.
    });
  } catch (...) {
    --queued_commands;
    throw;
  }
}

void RtcEngine::Impl::require_owner() const {
  if (!control->isCurrent()) {
    throw std::logic_error("RTC control requires its task queue");
  }
  if (closed) {
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed, "RTC engine closed");
  }
}

MediaModule& RtcEngine::Impl::require_media() const {
  require_owner();
  if (!media) {
    throw SDKError(ErrorDomain::media, ErrorCode::unsupported, "RTC media module is not enabled");
  }
  return *media;
}

void RtcEngine::Impl::clear_tracks() {
  pipelines->clear();
}

void RtcEngine::post_control(std::function<void()> task) {
  impl_->dispatch(std::move(task));
}

bool RtcEngine::on_control_thread() const {
  return impl_->control_is_current();
}

bool RtcEngine::media_enabled() const {
  return impl_->has_media;
}

AsyncResult<void> RtcEngine::join(RoomOptions options) {
  return impl_->submit([state = impl_.get(), options = std::move(options)]() mutable {
    state->require_owner();
    const auto connection = state->room->snapshot().state;
    if (connection == ConnectionState::failed || connection == ConnectionState::closed) {
      state->clear_tracks();
    }
    return state->room->join(std::move(options));
  });
}

AsyncResult<void> RtcEngine::leave() {
  return impl_->submit([state = impl_.get()] {
    state->require_owner();
    state->clear_tracks();
    return state->room->leave();
  });
}

AsyncResult<void> RtcEngine::close() {
  return impl_->submit([state = impl_.get()] {
    state->closed = true;
    state->clear_tracks();
    return state->room->leave();
  });
}

MediaOperation RtcEngine::start_media(MediaPublishSpec spec, std::optional<MediaKind> expected) {
  auto track = std::shared_ptr<MediaTrack>(new MediaTrack(spec.track));
  auto submitted =
      impl_->submit([state = impl_.get(), spec = std::move(spec), expected, track]() mutable {
        if (expected && spec.track.kind != *expected) {
          throw std::invalid_argument("Publication kind does not match the requested media API");
        }
        auto operation = state->require_media().publish_media(std::move(spec));
        track->bind_pending(std::move(operation.track));
        return std::move(operation.ready);
      });
  auto ready = std::move(submitted).then([](auto task) { task(); },
                                         [track](AsyncResult<TrackId> result) {
                                           try {
                                             return result.get();
                                           } catch (...) {
                                             track->fail_pending(std::current_exception());
                                             throw;
                                           }
                                         });
  return {std::move(ready), std::move(track)};
}

MediaOperation RtcEngine::publish_media(MediaPublishSpec spec) {
  return start_media(std::move(spec));
}
MediaOperation RtcEngine::publish_video(MediaPublishSpec spec) {
  return start_media(std::move(spec), MediaKind::video);
}
MediaOperation RtcEngine::publish_audio(MediaPublishSpec spec) {
  return start_media(std::move(spec), MediaKind::audio);
}

MediaOperation RtcEngine::subscribe_media(MediaSubscribeSpec spec) {
  auto track = std::shared_ptr<MediaTrack>(new MediaTrack(spec.track));
  auto submitted = impl_->submit([state = impl_.get(), spec = std::move(spec), track]() mutable {
    auto operation = state->require_media().subscribe_media(std::move(spec));
    track->bind_pending(std::move(operation.track));
    return std::move(operation.ready);
  });
  auto ready = std::move(submitted).then([](auto task) { task(); },
                                         [track](AsyncResult<TrackId> result) {
                                           try {
                                             return result.get();
                                           } catch (...) {
                                             track->fail_pending(std::current_exception());
                                             throw;
                                           }
                                         });
  return {std::move(ready), std::move(track)};
}

AsyncResult<std::shared_ptr<SharedMediaInput>> RtcEngine::create_shared_input(
    TrackDescriptor format, MediaInput input) {
  return impl_->submit(
      [state = impl_.get(), format = std::move(format), input = std::move(input)]() mutable {
        return state->require_media().create_shared_input(std::move(format), std::move(input));
      });
}

AsyncResult<TrackId> RtcEngine::publish(TrackDescriptor descriptor) {
  return impl_->submit([state = impl_.get(), descriptor = std::move(descriptor)]() mutable {
    state->require_owner();
    return state->pipelines->publish(std::move(descriptor));
  });
}

RtcGraphApi RtcEngine::graph() {
  return RtcGraphApi(*this);
}

AsyncResult<TrackId> RtcGraphApi::publish(TrackDescriptor descriptor,
                                          std::shared_ptr<graphflow::core::GraphNode> sink) {
  return engine_->impl_->submit([state = engine_->impl_.get(), descriptor = std::move(descriptor),
                                 sink = std::move(sink)]() mutable {
    state->require_owner();
    return state->pipelines->publish(std::move(descriptor), std::move(sink));
  });
}

AsyncResult<TrackId> RtcGraphApi::subscribe(TrackDescriptor descriptor,
                                            std::shared_ptr<graphflow::core::GraphNode> consumer) {
  return engine_->impl_->submit([state = engine_->impl_.get(), descriptor = std::move(descriptor),
                                 consumer = std::move(consumer)]() mutable {
    state->require_owner();
    return state->pipelines->subscribe(std::move(descriptor), std::move(consumer));
  });
}

AsyncResult<TrackId> RtcGraphApi::publish_graph(TrackDescriptor descriptor,
                                                PublishGraphFactory factory) {
  return engine_->impl_->submit([state = engine_->impl_.get(), descriptor = std::move(descriptor),
                                 factory = std::move(factory)]() mutable {
    state->require_owner();
    return state->pipelines->publish_graph(std::move(descriptor), std::move(factory));
  });
}

AsyncResult<TrackId> RtcGraphApi::subscribe_graph(TrackDescriptor descriptor,
                                                  SubscribeGraphFactory factory) {
  return engine_->impl_->submit([state = engine_->impl_.get(), descriptor = std::move(descriptor),
                                 factory = std::move(factory)]() mutable {
    state->require_owner();
    return state->pipelines->subscribe_graph(std::move(descriptor), std::move(factory));
  });
}

AsyncResult<void> RtcEngine::unpublish(const TrackId& id) {
  return impl_->submit([state = impl_.get(), id] {
    state->require_owner();
    return state->pipelines->unpublish(id);
  });
}

AsyncResult<void> RtcEngine::unsubscribe(const TrackId& id) {
  return impl_->submit([state = impl_.get(), id] {
    state->require_owner();
    return state->pipelines->unsubscribe(id);
  });
}

AsyncResult<void> RtcEngine::set_muted(const TrackId& id, bool muted, ControlOptions options) {
  return impl_->submit([state = impl_.get(), id, muted, options] {
    state->require_owner();
    return state->room->set_muted(id, muted, options);
  });
}

AsyncResult<void> RtcEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                                ControlOptions options) {
  return impl_->submit([state = impl_.get(), id, demand, options] {
    state->require_owner();
    return state->room->set_receive_demand(id, demand, options);
  });
}

WriteStatus RtcEngine::write(const TrackId& id, EncodedFrame frame) {
  return impl_->pipelines->write(id, std::move(frame));
}

TrackSnapshot RtcEngine::publication(const TrackId& id) const {
  return impl_->pipelines->publication(id);
}

SenderFeedback RtcEngine::feedback(const TrackId& id) const {
  return impl_->pipelines->feedback(id);
}

void RtcEngine::set_callbacks(Callbacks callbacks) {
  auto active = std::make_shared<std::atomic<bool>>(true);
  impl_->call([&] {
    if (impl_->callback_active) {
      *impl_->callback_active = false;
    }
    impl_->callback_active = active;
    impl_->pipelines->set_failure_handler(
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
  });
  // Keep control available while fencing callbacks that may await control calls.
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
  impl_->room->set_event_handler(std::move(event));
}

const RoomView& RtcEngine::room() const {
  return impl_->room_view;
}

}  // namespace livekit::sdk::rtc::engine
