#include "engines/rtc/rtc_execution_context.hpp"
#include "backends/rtc/room_engine.hpp"
#include "backends/rtc/track_control.hpp"
#include "backends/rtc/endpoint.hpp"
#include "backends/rtc/async_control.hpp"
#include "backends/rtc/lifecycle_executor.hpp"
#include "backends/rtc/bounded_events.hpp"
#include "support/task_dispatcher.hpp"
#include <algorithm>
#include <thread>

namespace livekit::sdk::rtc {
bool is_callback_thread() noexcept {
  return detail::callback_depth || detail::active_session || detail::worker_depth;
}

namespace detail {
namespace {
std::exception_ptr error(std::string message, ErrorCode code = ErrorCode::session_closed) {
  return std::make_exception_ptr(SDKError(ErrorDomain::lifecycle, code, std::move(message)));
}
}  // namespace

struct RtcExecutionState : std::enable_shared_from_this<RtcExecutionState> {
  std::shared_ptr<SessionControl> control;
  std::unique_ptr<RoomEngine> room;
  std::shared_ptr<TrackControl> tracks = std::make_shared<TrackControl>();
  std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup =
      std::make_shared<livekit::sdk::detail::WakeEvent>();
  std::mutex controller_mutex;
  std::jthread worker;
  LifecycleExecutor lifecycle;
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::shared_ptr<Endpoint>, std::exception_ptr> graph_failures, completed_removals;
  std::deque<RoomEvent> events;
  size_t max_events = 256;
  bool shutting_down = false;
  std::unique_ptr<vi::TaskQueue> notifications = vi::TaskQueue::create("rtc.notifications");
  livekit::sdk::detail::TaskDispatcher notification_dispatch;

  struct ServiceFailure {
    std::mutex mutex;
    std::exception_ptr cause;
  };

  std::shared_ptr<ServiceFailure> service_failure;
  // Notification/control workers never access the adapter-specific pointer,
  // which is replaced by the controller on rejoin.
  ServiceFailure notification_failure;

  void report_notification_failure(std::exception_ptr cause) noexcept {
    try {
      std::lock_guard lock(notification_failure.mutex);
      if (!notification_failure.cause) {
        notification_failure.cause = cause;
      }
      wakeup->signal();
    } catch (...) {
    }
  }

  void attach_service_notifications(RoomTransport& adapter) {
    service_failure = std::make_shared<ServiceFailure>();
    adapter.attach_notifications(
        [dispatch = notification_dispatch](auto task) {
          dispatch([task = std::move(task)] {
            CallbackScope scope;
            task();
          });
        },
        [failure = service_failure, event = wakeup](std::exception_ptr cause) {
          std::lock_guard lock(failure->mutex);
          if (!failure->cause) {
            failure->cause = cause;
          }
          event->signal();
        });
  }

  void handle_service_failure() {
    std::exception_ptr error;
    {
      std::lock_guard lock(service_failure->mutex);
      error = std::exchange(service_failure->cause, {});
    }
    {
      std::lock_guard lock(notification_failure.mutex);
      if (auto failure = std::exchange(notification_failure.cause, {})) {
        error = failure;
      }
    }
    if (error) {
      room->notification_failed(error);
    }
  }

  std::function<void(const RoomEvent&)> event_handler;
  bool notification_scheduled = false;
  size_t extra_notifications = 0;

  void notification_failed() noexcept {
    try {
      {
        std::lock_guard lock(mutex);
        event_handler = {};
      }
      report_notification_failure(
          error("RTC event callback threw; handler disabled", ErrorCode::callback_failed));
    } catch (...) {
      // The queue boundary must not propagate callback/dispatch exceptions.
    }
  }

  void schedule_events_locked() {
    if (notification_scheduled || !event_handler || events.empty()) {
      return;
    }
    notification_scheduled = true;
    try {
      notifications->postTask([this] {
        try {
          for (size_t count = 0; count < 32; ++count) {
            std::function<void(const RoomEvent&)> callback;
            std::optional<RoomEvent> event;
            {
              std::lock_guard lock(mutex);
              if (!event_handler || events.empty()) {
                break;
              }
              callback = event_handler;
              event = std::move(events.front());
              events.pop_front();
            }
            try {
              CallbackScope callback_scope;
              callback(*event);
            } catch (...) {
              notification_failed();
              break;
            }
          }
          std::lock_guard lock(mutex);
          notification_scheduled = false;
          schedule_events_locked();
          changed.notify_all();
        } catch (...) {
          {
            std::lock_guard lock(mutex);
            notification_scheduled = false;
            changed.notify_all();
          }
          notification_failed();
        }
      });
    } catch (...) {
      notification_scheduled = false;
      report_notification_failure(std::current_exception());
    }
  }

  void set_handler(std::function<void(const RoomEvent&)> handler) {
    {
      std::lock_guard lock(mutex);
      event_handler = std::move(handler);
      schedule_events_locked();
    }
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return !notification_scheduled && extra_notifications == 0; });
  }

  void post_notification(std::function<void()> callback, bool completion = false) {
    {
      std::lock_guard lock(mutex);
      if (!completion && extra_notifications >= 256) {
        report_notification_failure(
            error("RTC notification task queue exhausted", ErrorCode::resource_exhausted));
        return;
      }
      ++extra_notifications;
    }
    try {
      notifications->postTask([this, callback = std::move(callback), completion] {
        try {
          CallbackScope callback_scope;
          // Events are normally dispatched in batches. Drain their already
          // queued tail before a completion barrier, without making control or
          // lifecycle workers wait for application code.
          if (completion) {
            for (;;) {
              std::function<void(const RoomEvent&)> handler;
              std::optional<RoomEvent> event;
              {
                std::lock_guard lock(mutex);
                if (!event_handler || events.empty()) {
                  break;
                }
                handler = event_handler;
                event = std::move(events.front());
                events.pop_front();
              }
              try {
                handler(*event);
              } catch (...) {
                notification_failed();
              }
            }
          }
          callback();
        } catch (...) {
          notification_failed();
        }
        std::lock_guard lock(mutex);
        --extra_notifications;
        changed.notify_all();
      });
    } catch (...) {
      std::lock_guard lock(mutex);
      --extra_notifications;
      changed.notify_all();
      report_notification_failure(std::current_exception());
    }
  }

  explicit RtcExecutionState(std::shared_ptr<SessionControl> context)
      : control(std::move(context)) {
    notification_dispatch.bind(*notifications);
    service_failure = std::make_shared<ServiceFailure>();
    tracks->control = control;
  }

  // The gate belongs to the engine, not the protocol component. No thread
  // identity is required: initiation, reads and polling are serialized here.
  template <class T, class Function>
  auto initiate(Function function) {
    using Value = std::conditional_t<std::is_void_v<T>, ErrorCode, Result<T>>;
    auto source = std::make_shared<CompletionSource<Value>>();
    auto result = source->get_result();
    auto done = [source](Result<T> completed) mutable {
      if constexpr (std::is_void_v<T>) {
        if (completed) {
          source->set_value(ErrorCode::ok);
        } else {
          source->set_exception(std::make_exception_ptr(SDKError(completed.failure_record())));
        }
      } else {
        source->set_value(std::move(completed));
      }
    };
    try {
      std::lock_guard lock(controller_mutex);
      function(*room, done);
    } catch (...) {
      done(Result<T>::failure(describe_failure(std::current_exception())));
    }
    return result;
  }

  bool emit(RoomEvent event, bool terminal, bool fatal_overflow) {
    std::lock_guard lock(mutex);
    if (terminal && events.size() >= max_events) {
      events.pop_front();
    }
    const auto result = enqueue_event(
        events, std::move(event), max_events, RoomEventKind::statistics,
        [](const auto& queued, const auto& incoming) {
          return incoming.kind == RoomEventKind::session_changed &&
                 std::any_of(queued.begin(), queued.end(), [&](const auto& value) {
                   return value.kind == incoming.kind && value.epoch == incoming.epoch;
                 });
        });
    if (result == EventAdmission::overflow) {
      if (fatal_overflow) {
        report_notification_failure(
            error("RTC room event queue exhausted", ErrorCode::resource_exhausted));
      }
      return false;
    }
    if (result == EventAdmission::queued) {
      schedule_events_locked();
    }
    return true;
  }

  void report_failure(std::shared_ptr<Endpoint> endpoint, std::exception_ptr failure) {
    std::lock_guard lock(mutex);
    if (!shutting_down) {
      graph_failures.try_emplace(std::move(endpoint), std::move(failure));
      wakeup->signal();
    }
  }

  void initialize(RoomEngine::TransportFactory factory) {
    auto weak = weak_from_this();
    tracks->set_muted_async = [weak](TrackId id, bool muted, ControlOptions options,
                                     std::shared_ptr<Endpoint> expected) {
      auto self = weak.lock();
      if (!self) {
        throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed, "RTC engine destroyed");
      }
      return self->initiate<void>([&](RoomEngine& room, auto done) {
        room.set_muted(id, muted, options, std::move(done), std::move(expected));
      });
    };
    tracks->set_receive_demand_async = [weak](TrackId id, ReceiveDemand demand,
                                              ControlOptions options,
                                              std::shared_ptr<Endpoint> expected) {
      auto self = weak.lock();
      if (!self) {
        throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed, "RTC engine destroyed");
      }
      return self->initiate<void>([&](RoomEngine& room, auto done) {
        room.set_receive_demand(id, demand, options, std::move(done), std::move(expected));
      });
    };
    tracks->close_track_async = [weak](std::shared_ptr<Endpoint> endpoint) {
      auto self = weak.lock();
      if (!self) {
        throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed, "RTC engine destroyed");
      }
      return self->initiate<void>(
          [&](RoomEngine& room, auto done) { room.close_track(endpoint, std::move(done)); });
    };
    tracks->request_keyframe_async = [weak](std::shared_ptr<Endpoint> endpoint) {
      auto self = weak.lock();
      if (!self) {
        throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed, "RTC engine destroyed");
      }
      return self->initiate<void>(
          [&](RoomEngine& room, auto done) { room.request_keyframe(endpoint, std::move(done)); });
    };
    tracks->report_failure = [weak](auto endpoint, auto failure) {
      if (auto self = weak.lock()) {
        self->report_failure(std::move(endpoint), std::move(failure));
      }
    };
    RoomEngine::Hooks hooks;
    hooks.wakeup = wakeup;
    hooks.tracks = tracks;
    hooks.event = [this](auto event, bool terminal, bool fatal) {
      return emit(std::move(event), terminal, fatal);
    };
    hooks.reset_events = [this](size_t capacity) {
      std::lock_guard lock(mutex);
      events.clear();
      max_events = capacity;
    };
    hooks.attach_services = [this](RoomTransport& adapter) {
      attach_service_notifications(adapter);
    };
    hooks.removal = [weak](std::shared_ptr<Endpoint> endpoint) -> RoomTransport::Removed {
      return [weak, endpoint = std::move(endpoint)](std::exception_ptr failure) {
        if (auto self = weak.lock()) {
          std::lock_guard lock(self->mutex);
          if (!self->shutting_down) {
            self->completed_removals.emplace(endpoint, std::move(failure));
            self->wakeup->signal();
          }
        }
      };
    };
    room = std::make_unique<RoomEngine>(std::move(factory), std::move(hooks));
    worker = std::jthread([this](std::stop_token stop) { run(stop); });
  }

  void run(std::stop_token stop) {
    WorkerScope scope;
    while (!stop.stop_requested()) {
      const auto observed = wakeup->revision();
      std::optional<SteadyClock::time_point> deadline;
      {
        std::lock_guard lock(controller_mutex);
        std::map<std::shared_ptr<Endpoint>, std::exception_ptr> failures, detached;
        {
          std::lock_guard inbox_lock(mutex);
          failures.swap(graph_failures);
          detached.swap(completed_removals);
        }
        try {
          handle_service_failure();
          for (auto& [endpoint, failure] : detached) {
            room->finish_removal(endpoint, failure);
          }
          for (auto& [endpoint, failure] : failures) {
            room->report_failure(endpoint, failure);
          }
          room->poll(control->interrupted.load());
          deadline = room->next_deadline();
        } catch (...) {
          room->notification_failed(std::current_exception());
        }
      }
      wakeup->wait(observed, stop, deadline);
    }
    std::lock_guard lock(controller_mutex);
    room->leave([](Result<void>) {});
  }

  void shutdown() {
    {
      std::lock_guard lock(mutex);
      shutting_down = true;
    }
    worker.request_stop();
    wakeup->signal();
    if (worker.joinable()) {
      worker.join();
    }
  }

  ~RtcExecutionState() {
    shutdown();
    set_handler({});
    room.reset();
    notification_dispatch.close();
    notifications.reset();
  }
};

}  // namespace detail

PublishedTrack::PublishedTrack(std::shared_ptr<detail::Endpoint> endpoint,
                               std::weak_ptr<detail::TrackControl> session)
    : endpoint_(std::move(endpoint)), session_(std::move(session)) {}

const TrackDescriptor& PublishedTrack::descriptor() const {
  return endpoint_->descriptor;
}

TrackSnapshot PublishedTrack::snapshot() const {
  return endpoint_->snapshot();
}

SenderFeedback PublishedTrack::feedback() const {
  return endpoint_->feedback();
}

std::optional<SenderFeedback> PublishedTrack::wait_feedback(Epoch epoch, uint64_t revision,
                                                            std::stop_token stop) const {
  return endpoint_->wait_feedback(epoch, revision, stop);
}

WriteStatus PublishedTrack::try_write(EncodedFrame frame) {
  return endpoint_->write(std::move(frame));
}

ErrorCode PublishedTrack::set_muted(bool muted, ControlOptions options) {
  return detail::control_boundary([&] {
    if (auto session = session_.lock()) {
      auto pending = [&] {
        detail::OperationGuard guard(*session->control);
        return session->set_muted_async(endpoint_->descriptor.id, muted, options, endpoint_);
      }();
      return pending.get();
    }
    return ErrorCode::session_closed;
  });
}

void PublishedTrack::report_failure(std::exception_ptr failure) {
  if (auto session = session_.lock()) {
    session->report_failure(endpoint_, failure);
  }
}

std::shared_ptr<detail::SessionControl> PublishedTrack::operation_control() const {
  auto session = session_.lock();
  if (!session) {
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed, "RTC room destroyed");
  }
  return session->control;
}

ErrorCode PublishedTrack::close() {
  return detail::control_boundary([&] {
    if (auto session = session_.lock()) {
      auto pending = [&] {
        detail::OperationGuard guard(*session->control);
        return session->close_track_async(endpoint_);
      }();
      return pending.get();
    }
    return ErrorCode::session_closed;
  });
}

ErrorCode PublishedTrack::close_locked() {
  if (auto session = session_.lock()) {
    return session->close_track(endpoint_);
  }
  return ErrorCode::session_closed;
}

SubscribedTrack::SubscribedTrack(std::shared_ptr<detail::Endpoint> endpoint,
                                 std::weak_ptr<detail::TrackControl> session)
    : endpoint_(std::move(endpoint)), session_(std::move(session)) {}

const TrackDescriptor& SubscribedTrack::descriptor() const {
  return endpoint_->descriptor;
}

TrackSnapshot SubscribedTrack::snapshot() const {
  return endpoint_->snapshot();
}

ReadResult SubscribedTrack::read(std::chrono::milliseconds timeout, std::stop_token stop) {
  return endpoint_->read(timeout, stop);
}

ReadResult SubscribedTrack::wait_frame(std::stop_token stop) {
  return endpoint_->wait_frame(stop);
}

ErrorCode SubscribedTrack::request_keyframe() {
  return detail::control_boundary([&] {
    if (auto session = session_.lock()) {
      detail::OperationGuard guard(*session->control);
      return session->request_keyframe(endpoint_);
    }
    return ErrorCode::session_closed;
  });
}

ErrorCode SubscribedTrack::set_demand(ReceiveDemand demand, ControlOptions options) {
  return detail::control_boundary([&] {
    if (auto session = session_.lock()) {
      auto pending = [&] {
        detail::OperationGuard guard(*session->control);
        return session->set_receive_demand_async(endpoint_->descriptor.id, demand, options,
                                                 endpoint_);
      }();
      return pending.get();
    }
    return ErrorCode::session_closed;
  });
}

void SubscribedTrack::report_failure(std::exception_ptr failure) {
  if (auto session = session_.lock()) {
    session->report_failure(endpoint_, failure);
  }
}

std::shared_ptr<detail::SessionControl> SubscribedTrack::operation_control() const {
  auto session = session_.lock();
  if (!session) {
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed, "RTC room destroyed");
  }
  return session->control;
}

ErrorCode SubscribedTrack::close() {
  return detail::control_boundary([&] {
    if (auto session = session_.lock()) {
      auto pending = [&] {
        detail::OperationGuard guard(*session->control);
        return session->close_track_async(endpoint_);
      }();
      return pending.get();
    }
    return ErrorCode::session_closed;
  });
}

ErrorCode SubscribedTrack::close_locked() {
  if (auto session = session_.lock()) {
    return session->close_track(endpoint_);
  }
  return ErrorCode::session_closed;
}

void SubscribedTrack::request_keyframe_async() {
  endpoint_->require_keyframe();
}

namespace detail {
RtcExecutionContext::RtcExecutionContext(RoomEngine::TransportFactory factory)
    : state_(std::make_shared<RtcExecutionState>(std::make_shared<SessionControl>())) {
  state_->initialize(std::move(factory));
  auto* state = state_.get();
  state_->control->async = std::make_shared<AsyncControl>(
      *state_->control, [state](auto task) { state->post_notification(std::move(task), true); },
      [wakeup = state_->wakeup] { wakeup->signal(); },
      [state](auto failure) { state->report_notification_failure(failure); });
}

RtcExecutionContext::~RtcExecutionContext() {
  state_->control->async->shutdown();
  state_->lifecycle.shutdown();
  state_->shutdown();
  state_->set_handler({});
}

std::shared_ptr<SessionControl> RtcExecutionContext::control() const {
  return state_->control;
}

LifecycleExecutor& RtcExecutionContext::lifecycle() {
  return state_->lifecycle;
}

void RtcExecutionContext::set_event_handler(std::function<void(const RoomEvent&)> handler) {
  state_->set_handler(std::move(handler));
}

void RtcExecutionContext::post_notification(std::function<void()> task) {
  state_->post_notification(std::move(task));
}

void RtcExecutionContext::access(const std::function<void(RoomEngine&)>& task) {
  std::lock_guard lock(state_->controller_mutex);
  task(*state_->room);
}

RoomSnapshot RtcExecutionContext::snapshot() const {
  std::lock_guard lock(state_->controller_mutex);
  return state_->room->snapshot();
}

Result<std::shared_ptr<RoomServices>> RtcExecutionContext::services() const {
  std::lock_guard lock(state_->controller_mutex);
  return state_->room->services();
}
}  // namespace detail
}  // namespace livekit::sdk::rtc
