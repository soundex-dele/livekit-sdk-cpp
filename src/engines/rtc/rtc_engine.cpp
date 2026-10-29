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
  call([this] { pipelines = std::make_unique<RtcPipelineEngine>(*room); });
  room->set_completion_wakeup([this] { completion_ready(); });
  call([this] { schedule_health(); });
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
  room->set_completion_wakeup({});
  try {
    call([this] {
      stopping = true;
      clear_tracks();
      media.reset();
      pipelines.reset();
    });
  } catch (...) {
  }
  control.reset();
  try {
    room->leave().get();
  } catch (...) {
  }
}

void RtcEngine::Impl::dispatch(std::function<void()> task) const {
  if (control->isCurrent()) {
    task();
    return;
  }
  if (queued_commands.fetch_add(1) >= 1024) {
    --queued_commands;
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                   "RTC control task queue exhausted");
  }
  try {
    control->postTask([this, task = std::move(task)] {
      --queued_commands;
      task();  // Always a packaged_task; exceptions become future failures.
    });
  } catch (...) {
    --queued_commands;
    throw;
  }
}

void RtcEngine::Impl::fail_control(std::exception_ptr failure) {
  control_failure = failure;
  try {
    clear_tracks();
  } catch (...) {
  }
}

void RtcEngine::Impl::completion_ready() {
  if (completion_queued.exchange(true)) {
    return;
  }
  control->postTask([this] {
    completion_queued = false;
    if (stopping || control_failure) {
      return;
    }
    try {
      pipelines->complete_operations();
    } catch (...) {
      fail_control(std::current_exception());
    }
  });
}

void RtcEngine::Impl::schedule_health() {
  // Legacy/custom nodes report failures through bounded snapshot checks. This
  // timer performs no receive, feedback, graph creation or media dispatch.
  control->postDelayedTask(
      [this] {
        if (stopping || control_failure) {
          return;
        }
        try {
          pipelines->check_health();
          schedule_health();
        } catch (...) {
          fail_control(std::current_exception());
        }
      },
      20);
}

void RtcEngine::Impl::require_owner() const {
  if (!control->isCurrent()) {
    throw std::logic_error("RTC control requires its task queue");
  }
  if (control_failure) {
    std::rethrow_exception(control_failure);
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

bool RtcEngine::media_enabled() const {
  return impl_->call([&] {
    impl_->require_owner();
    return bool(impl_->media);
  });
}

std::future<void> RtcEngine::join(RoomOptions options) {
  return impl_->call([&] {
    impl_->require_owner();
    const auto state = impl_->room->snapshot().state;
    if (state == ConnectionState::failed || state == ConnectionState::closed) {
      impl_->clear_tracks();
    }
    return impl_->room->join(std::move(options));
  });
}

std::future<void> RtcEngine::leave() {
  return impl_->call([&] {
    impl_->require_owner();
    impl_->clear_tracks();
    return impl_->room->leave();
  });
}

std::future<void> RtcEngine::close() {
  return impl_->call([&] {
    impl_->closed = true;
    impl_->clear_tracks();
    return impl_->room->leave();
  });
}

MediaOperation RtcEngine::publish_media(MediaPublishSpec spec) {
  return impl_->call([&] { return impl_->require_media().publish_media(std::move(spec)); });
}

MediaOperation RtcEngine::publish_video(MediaPublishSpec spec) {
  if (spec.track.kind != MediaKind::video) {
    throw std::invalid_argument("Expected video publication");
  }
  return publish_media(std::move(spec));
}

MediaOperation RtcEngine::publish_audio(MediaPublishSpec spec) {
  if (spec.track.kind != MediaKind::audio) {
    throw std::invalid_argument("Expected audio publication");
  }
  return publish_media(std::move(spec));
}

MediaOperation RtcEngine::subscribe_media(MediaSubscribeSpec spec) {
  return impl_->call([&] { return impl_->require_media().subscribe_media(std::move(spec)); });
}

std::shared_ptr<SharedMediaInput> RtcEngine::create_shared_input(TrackDescriptor format,
                                                                 MediaInput input) {
  return impl_->call([&] {
    return impl_->require_media().create_shared_input(std::move(format), std::move(input));
  });
}

std::future<TrackId> RtcEngine::publish(TrackDescriptor descriptor) {
  return impl_->call([&] {
    impl_->require_owner();
    return impl_->pipelines->publish(std::move(descriptor));
  });
}

RtcGraphApi RtcEngine::graph() {
  impl_->call([&] { impl_->require_owner(); });
  return RtcGraphApi(*this);
}

std::future<TrackId> RtcGraphApi::publish(TrackDescriptor descriptor,
                                          std::shared_ptr<graphflow::core::GraphNode> sink) {
  return engine_->impl_->call([&] {
    engine_->impl_->require_owner();
    return engine_->impl_->pipelines->publish(std::move(descriptor), std::move(sink));
  });
}

std::future<TrackId> RtcGraphApi::subscribe(TrackDescriptor descriptor,
                                            std::shared_ptr<graphflow::core::GraphNode> consumer) {
  return engine_->impl_->call([&] {
    engine_->impl_->require_owner();
    return engine_->impl_->pipelines->subscribe(std::move(descriptor), std::move(consumer));
  });
}

std::future<TrackId> RtcGraphApi::publish_graph(TrackDescriptor descriptor,
                                                PublishGraphFactory factory) {
  return engine_->impl_->call([&] {
    engine_->impl_->require_owner();
    return engine_->impl_->pipelines->publish_graph(std::move(descriptor), std::move(factory));
  });
}

std::future<TrackId> RtcGraphApi::subscribe_graph(TrackDescriptor descriptor,
                                                  SubscribeGraphFactory factory) {
  return engine_->impl_->call([&] {
    engine_->impl_->require_owner();
    return engine_->impl_->pipelines->subscribe_graph(std::move(descriptor), std::move(factory));
  });
}

std::future<void> RtcEngine::unpublish(const TrackId& id) {
  return impl_->call([&] {
    impl_->require_owner();
    auto result = impl_->pipelines->unpublish(id);
    return result;
  });
}

std::future<void> RtcEngine::unsubscribe(const TrackId& id) {
  return impl_->call([&] {
    impl_->require_owner();
    auto result = impl_->pipelines->unsubscribe(id);
    return result;
  });
}

std::future<void> RtcEngine::set_muted(const TrackId& id, bool muted, ControlOptions options) {
  return impl_->call([&] {
    impl_->require_owner();
    return impl_->room->set_muted(id, muted, options);
  });
}

std::future<void> RtcEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                                ControlOptions options) {
  return impl_->call([&] {
    impl_->require_owner();
    return impl_->room->set_receive_demand(id, demand, options);
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
                  handler(GraphFailure{failure.id, failure.publication, failure.message});
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
