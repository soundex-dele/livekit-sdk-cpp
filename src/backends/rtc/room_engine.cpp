#include "livekit/rtc/room_engine.hpp"
#include "backends/rtc/endpoint.hpp"
#include "backends/rtc/control_context.hpp"
#include "backends/rtc/async_control.hpp"
#include "backends/rtc/bounded_events.hpp"
#include "backends/rtc/subscription_policy.hpp"
#include "livekit/error.hpp"
#include "support/utf8.hpp"
#include "support/task_dispatcher.hpp"
#include "support/ready_set.hpp"
#include "support/deadline_index.hpp"
#include "utoolkit/task_queue/task_queue.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <thread>

namespace livekit::sdk::rtc {
namespace detail {
namespace {
std::exception_ptr error(std::string message, ErrorCode code = ErrorCode::session_closed) {
  return std::make_exception_ptr(SDKError(ErrorDomain::lifecycle, code, std::move(message)));
}

}  // namespace

// Stack-owned state for a synchronous protocol wait. Only the calling thread
// accesses it while pumping transport events under the controller mutex.
template <class T>
struct PendingOperation {
  using Return = std::conditional_t<std::is_void_v<T>, ErrorCode, Result<T>>;
  std::optional<Return> result;

  template <class... Args>
  void succeed(Args&&... args) {
    if (!result) {
      if constexpr (std::is_void_v<T>) {
        result.emplace(ErrorCode::ok);
      } else {
        result.emplace(Result<T>::success(std::forward<Args>(args)...));
      }
    }
  }

  void fail(std::exception_ptr failure) {
    if (!result) {
      if constexpr (std::is_void_v<T>) {
        control_failure = describe_failure(failure);
        result.emplace(control_failure->code);
      } else {
        result.emplace(Result<T>::failure(describe_failure(failure)));
      }
    }
  }
};

struct RoomEngineState : std::enable_shared_from_this<RoomEngineState> {
  RoomEngineState(RoomEngine::TransportFactory factory, std::shared_ptr<SessionControl> context)
      : transport_factory(std::move(factory)), control(std::move(context)) {
    notifications = vi::TaskQueue::create("rtc.room.notifications");
    notification_dispatch.bind(*notifications);
    service_failure = std::make_shared<ServiceFailure>();
    try {
      if (transport_factory) {
        transport = transport_factory();
      }
      if (transport) {
        transport->attach_wakeup(wakeup);
        services = transport->services();
        attach_service_notifications(*transport);
      }
    } catch (...) {
      initialization_failure = std::current_exception();
      services.reset();
      transport.reset();
    }
    worker = std::jthread([this](std::stop_token stop) {
      CallbackScope worker_scope;
      run(stop);
    });
  }

  ~RoomEngineState() {
    set_handler({});
    shutdown();
    notification_dispatch.close();
    notifications.reset();
  }

  struct Entry {
    std::shared_ptr<Endpoint> endpoint;
    SteadyClock::time_point deadline;
    std::function<void(std::exception_ptr)> completion;
    SteadyClock::time_point last_pli{};
  };

  struct Removal {
    std::shared_ptr<Endpoint> endpoint;
    PendingOperation<void>* waiting = nullptr;
  };

  using Key = std::pair<bool, TrackId>;
  enum class WorkKind { track, control };
  using WorkKey = std::pair<WorkKind, uint64_t>;
  RoomEngine::TransportFactory transport_factory;
  std::unique_ptr<RoomTransport> transport;
  std::exception_ptr initialization_failure;
  std::shared_ptr<RoomServices> services;
  std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup =
      std::make_shared<livekit::sdk::detail::WakeEvent>();
  std::shared_ptr<livekit::sdk::detail::ReadySet<WorkKey>> ready_work =
      std::make_shared<livekit::sdk::detail::ReadySet<WorkKey>>(wakeup, 1024 + 128);
  std::jthread worker;
  std::mutex controller_mutex;
  std::shared_ptr<SessionControl> control;
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  // At most one report per registered graph, independent of room event capacity.
  std::map<std::shared_ptr<Endpoint>, std::exception_ptr> graph_failures;
  std::map<std::shared_ptr<Endpoint>, std::exception_ptr> completed_removals;
  std::deque<RoomEvent> events;
  std::unique_ptr<vi::TaskQueue> notifications;
  livekit::sdk::detail::TaskDispatcher notification_dispatch;

  struct ServiceFailure {
    std::mutex mutex;
    std::exception_ptr cause;
  };

  std::shared_ptr<ServiceFailure> service_failure;

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
    if (error && connection != ConnectionState::failed &&
        (connection != ConnectionState::closed ||
         describe_failure(error)->code == ErrorCode::callback_failed)) {
      stop_all(ConnectionState::failed, describe_failure(error)->message, error);
    }
  }

  ObserverSlot<RoomObserver> room_observer;
  ObserverSlot<PublicationObserver> publication_observer;
  ObserverSlot<SubscriptionObserver> subscription_observer;
  std::function<void(const RoomEvent&)> event_handler;
  bool notification_scheduled = false;
  size_t extra_notifications = 0;

  void notification_failed() noexcept {
    try {
      {
        std::lock_guard lock(mutex);
        event_handler = {};
      }
      std::lock_guard failure_lock(service_failure->mutex);
      service_failure->cause =
          error("RTC event callback threw; handler disabled", ErrorCode::callback_failed);
      wakeup->signal();
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
      std::lock_guard failure_lock(service_failure->mutex);
      service_failure->cause = std::current_exception();
      wakeup->signal();
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
        std::lock_guard failure_lock(service_failure->mutex);
        service_failure->cause =
            error("RTC notification task queue exhausted", ErrorCode::resource_exhausted);
        wakeup->signal();
        return;
      }
      ++extra_notifications;
    }
    try {
      notifications->postTask([this, callback = std::move(callback)] {
        try {
          CallbackScope callback_scope;
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
      std::lock_guard failure_lock(service_failure->mutex);
      service_failure->cause = std::current_exception();
      wakeup->signal();
    }
  }

  bool shutting_down = false;
  RoomSnapshot status;
  // The controller exclusively owns everything below this line.
  RoomOptions options;
  ConnectionState connection = ConnectionState::disconnected;
  Epoch epoch = 0;
  uint64_t next_incarnation = 0;
  std::shared_ptr<QueueBudget> budget;
  std::map<Key, Entry> tracks;
  std::map<uint64_t, Key> track_keys;
  livekit::sdk::detail::DeadlineIndex<WorkKey> deadlines;
  size_t publication_count = 0, subscription_count = 0;
  uint64_t tracks_serviced = 0, controller_wakeups = 0;
  std::vector<Removal> removals;  // Controller-owned native detachment barriers.
  PendingOperation<void>* joining = nullptr;
  SteadyClock::time_point join_deadline{};

  struct PendingControl {
    PendingOperation<void>* pending;
    Epoch epoch;
    SteadyClock::time_point deadline;
    std::stop_token stop;
    std::function<void()> applied;
    std::unique_ptr<std::stop_callback<std::function<void()>>> cancellation;
  };

  uint32_t next_control = 0;
  std::map<uint32_t, PendingControl> controls;

  bool add_control(uint32_t id, PendingControl control) {
    auto* pending = control.pending;
    const WorkKey key{WorkKind::control, id};
    auto signal = ready_work->watch(key);
    try {
      control.cancellation = std::make_unique<std::stop_callback<std::function<void()>>>(
          control.stop, [signal] { signal->signal(); });
      deadlines.set(key, control.deadline);
      controls.emplace(id, std::move(control));
      return true;
    } catch (...) {
      deadlines.erase(key);
      ready_work->forget(key);
      pending->fail(std::current_exception());
      return false;
    }
  }

  void erase_control(uint32_t id) {
    deadlines.erase({WorkKind::control, id});
    ready_work->forget({WorkKind::control, id});
    controls.erase(id);
  }

  void erase_track(std::map<Key, Entry>::iterator it) {
    const auto incarnation = it->second.endpoint->binding().incarnation;
    deadlines.erase({WorkKind::track, incarnation});
    ready_work->forget({WorkKind::track, incarnation});
    track_keys.erase(incarnation);
    --(it->first.first ? publication_count : subscription_count);
    tracks.erase(it);
  }

  void interrupt_controls() {
    for (const auto& [id, pending] : controls) {
      deadlines.erase({WorkKind::control, id});
      ready_work->forget({WorkKind::control, id});
      pending.pending->fail(
          error("RTC control interrupted; remote outcome is unknown", ErrorCode::outcome_unknown));
    }
    controls.clear();
  }

  void shutdown() {
    {
      std::lock_guard lock(mutex);
      shutting_down = true;
    }
    worker.request_stop();
    wakeup->signal();
    if (worker.joinable())
      worker.join();
  }

  template <class Function, class T>
  void run_request(Function&& request, PendingOperation<T>& operation) {
    try {
      request();
    } catch (...) {
      operation.fail(std::current_exception());
    }
  }

  template <class T>
  auto wait(PendingOperation<T>& operation) {
    while (!operation.result) {
      const auto observed = wakeup->revision();
      if (control->interrupted.load()) {
        stop_all(ConnectionState::closed, "RTC operation cancelled by leave/close",
                 error("RTC operation cancelled by leave/close", ErrorCode::cancelled));
      } else {
        pump();
      }
      if (!operation.result) {
        wakeup->wait(observed, {}, next_deadline());
      }
    }
    wakeup->signal();  // Let background media servicing resume after the caller unlocks.
    return std::move(*operation.result);
  }

  void report_failure(std::shared_ptr<Endpoint> endpoint, std::exception_ptr failure) {
    std::lock_guard lock(mutex);
    if (shutting_down) {
      return;
    }
    graph_failures.try_emplace(std::move(endpoint), failure);
    wakeup->signal();
  }

  void refresh(std::string message = {}) {
    std::lock_guard lock(mutex);
    status.state = connection;
    status.epoch = epoch;
    status.capabilities = transport ? transport->capabilities() : BackendCapabilities{};
    if (connection == ConnectionState::closed || connection == ConnectionState::failed ||
        (status.session && status.session->epoch != epoch))
      status.session.reset();
    status.queued_bytes = budget ? budget->used.load() : 0;
    status.publications = publication_count;
    status.subscriptions = subscription_count;
    status.controller_wakeups = controller_wakeups;
    status.tracks_serviced = tracks_serviced;
    status.scheduled_deadlines = deadlines.size() + bool(joining);
    status.ready_work = ready_work->pending();
    if (!message.empty())
      status.error = std::move(message);
    changed.notify_all();
  }

  bool emit_track(RoomEvent event, bool publication, bool fatal_overflow = true) {
    event.publication = publication;
    return emit(std::move(event), fatal_overflow);
  }

  bool emit(RoomEvent event, bool fatal_overflow = true) {
    std::lock_guard lock(mutex);
    const auto result = enqueue_event(
        events, std::move(event), options.max_events, RoomEventKind::statistics,
        [](const auto& queued, const auto& incoming) {
          // A room state event is a hint to read the latest immutable snapshot.
          return incoming.kind == RoomEventKind::session_changed &&
                 std::any_of(queued.begin(), queued.end(), [&](const auto& value) {
                   return value.kind == incoming.kind && value.epoch == incoming.epoch;
                 });
        });
    if (result == EventAdmission::overflow) {
      if (fatal_overflow) {
        std::lock_guard failure_lock(service_failure->mutex);
        service_failure->cause =
            error("RTC room event queue exhausted", ErrorCode::resource_exhausted);
        wakeup->signal();
      }
      return false;
    }
    if (result != EventAdmission::queued) {
      return true;
    }
    schedule_events_locked();
    changed.notify_all();
    return true;
  }

  void stop_all(ConnectionState state, const std::string& message,
                std::exception_ptr cause = {}) noexcept {
    connection = state;
    interrupt_controls();
    const auto failure =
        normalized_failure(cause ? cause : error(message), {"room", {}, {}, epoch});
    const auto info = describe_failure(failure);
    auto joining_result = std::exchange(joining, nullptr);
    for (auto& [key, entry] : tracks) {
      entry.endpoint->finish(
          state == ConnectionState::failed ? TrackState::failed : TrackState::closed, message,
          state == ConnectionState::failed ? info : nullptr);
      if (entry.completion) {
        entry.completion(failure);
        entry.completion = {};
      }
    }
    tracks.clear();
    track_keys.clear();
    publication_count = subscription_count = 0;
    deadlines.clear();
    ready_work->clear();
    if (transport) {
      transport->stop();
    }
    // stop() is the final native barrier, including removals queued at shutdown.
    finish_all_removals();
    refresh(state == ConnectionState::failed ? message : "");
    // A terminal notification must remain observable even if the event consumer
    // overflowed. Track/data payloads are never retained in this event queue.
    std::lock_guard lock(mutex);
    if (events.size() >= options.max_events)
      events.pop_front();
    events.push_back(
        {state == ConnectionState::failed ? RoomEventKind::failed : RoomEventKind::closed,
         {},
         {},
         epoch,
         message});
    if (state == ConnectionState::failed) {
      events.back().failure = info;
    }
    schedule_events_locked();
    // Completion observes the terminal snapshot and transport shutdown barrier.
    if (joining_result) {
      joining_result->fail(failure);
    }
    changed.notify_all();
  }

  void dispatch(Entry& entry) {
    const auto binding = entry.endpoint->binding();
    if (binding.publication) {
      auto descriptor = entry.endpoint->descriptor;
      descriptor.muted = entry.endpoint->snapshot().muted;
      transport->publish(binding, descriptor);
    } else {
      auto descriptor = entry.endpoint->descriptor;
      descriptor.receive_demand = entry.endpoint->snapshot().receive_demand;
      transport->subscribe(binding, descriptor, entry.endpoint->queue_budget());
    }
  }

  void fail_track(const Key& key, std::exception_ptr failure, const std::string& message) {
    auto it = tracks.find(key);
    if (it == tracks.end())
      return;
    auto entry = it->second;
    erase_track(it);
    failure = normalized_failure(failure, {"track", key.second, {}, epoch});
    const auto info = describe_failure(failure);
    entry.endpoint->finish(TrackState::failed, info->message, info);
    if (entry.completion)
      entry.completion(failure);
    // Native track cleanup is best-effort here. A session-wide failure must be
    // reported by the adapter as interrupted/terminal_error, not inferred from
    // an individual send or codec negotiation failure.
    try {
      detach_endpoint(entry.endpoint);
    } catch (...) {
    }
    RoomEvent notification{RoomEventKind::track_failed, key.second, {}, epoch, info->message};
    notification.failure = info;
    emit_track(std::move(notification), key.first);
    refresh();
  }

  void add(std::shared_ptr<Endpoint> endpoint, std::function<void(std::exception_ptr)> completion) {
    if (connection != ConnectionState::connecting && connection != ConnectionState::connected &&
        connection != ConnectionState::reconnecting) {
      completion(error("Join the RTC room before adding tracks", ErrorCode::session_closed));
      return;
    }
    const Key key{endpoint->publication, endpoint->descriptor.id};
    if (tracks.contains(key)) {
      completion(error("Duplicate RTC track id", ErrorCode::invalid_argument));
      return;
    }
    if (tracks.size() >= options.max_tracks) {
      completion(error("RTC room track limit exceeded", ErrorCode::resource_exhausted));
      return;
    }
    auto [it, inserted] = tracks.emplace(
        key,
        Entry{endpoint, SteadyClock::now() + options.operation_timeout, std::move(completion)});
    ++(key.first ? publication_count : subscription_count);
    try {
      const auto incarnation = endpoint->binding().incarnation;
      track_keys.emplace(incarnation, key);
      endpoint->attach_wakeup(ready_work->watch({WorkKind::track, incarnation}));
      deadlines.set({WorkKind::track, incarnation}, it->second.deadline);
      if (connection == ConnectionState::connected)
        dispatch(it->second);
      refresh();
    } catch (...) {
      auto failed = std::move(it->second.completion);
      erase_track(it);
      failed(std::current_exception());
      return;
    }
  }

  std::shared_ptr<Endpoint> make_endpoint(TrackDescriptor descriptor, bool publication) {
    if (!budget) {
      return {};
    }
    validate_descriptor(descriptor, budget->limit, publication);
    BindingToken binding{descriptor.id, epoch, ++next_incarnation, publication};
    auto endpoint = std::make_shared<Endpoint>(std::move(descriptor), std::move(binding), budget);
    return endpoint;
  }

  ErrorCode set_muted(TrackId id, bool muted, ControlOptions opts,
                      std::shared_ptr<Endpoint> expected = {}) {
    std::lock_guard controller_lock(controller_mutex);
    PendingOperation<void> operation;
    auto* pending = &operation;
    const auto submitted = SteadyClock::now();
    run_request(
        [&] {
          if (opts.timeout.count() < 0 || opts.timeout > std::chrono::minutes(5)) {
            pending->fail(detail::error("Invalid mute timeout", ErrorCode::invalid_argument));
            return;
          }
          if (opts.stop.stop_requested()) {
            pending->fail(error("Mute cancelled before dispatch", ErrorCode::cancelled));
            return;
          }
          const auto deadline =
              submitted + (opts.timeout.count() ? opts.timeout : options.operation_timeout);
          if (SteadyClock::now() >= deadline) {
            pending->fail(error("Mute expired before dispatch", ErrorCode::timeout));
            return;
          }
          const auto it = tracks.find({true, id});
          if (connection != ConnectionState::connected || it == tracks.end() ||
              (expected && expected != it->second.endpoint) ||
              it->second.endpoint->snapshot().state != TrackState::ready) {
            pending->fail(
                error("Publication is not ready for mute control", ErrorCode::stale_handle));
            return;
          }
          if (controls.size() >= 128 || next_control == std::numeric_limits<uint32_t>::max()) {
            pending->fail(error("RTC control capacity exhausted", ErrorCode::resource_exhausted));
            return;
          }
          const auto endpoint = it->second.endpoint;
          const auto previous = endpoint->snapshot().muted;
          endpoint->set_muted(muted);  // Gate application admission before native dispatch.
          const auto operation = ++next_control;
          try {
            // Reserve the notification before dispatch. If it cannot be queued,
            // no native operation may survive request rejection.
            if (!emit_track({RoomEventKind::track_mute_changed, id, {}, epoch, {}}, true, false) ||
                !add_control(operation, PendingControl{pending, epoch, deadline, opts.stop})) {
              endpoint->set_muted(previous);
              pending->fail(
                  error("RTC mute notification admission failed", ErrorCode::resource_exhausted));
              return;
            }
            transport->set_muted({operation, endpoint->binding(), muted, deadline, opts.stop});
          } catch (...) {
            erase_control(operation);
            endpoint->set_muted(previous);
            pending->fail(std::current_exception());
          }
        },
        operation);
    return wait(operation);
  }

  ErrorCode set_receive_demand(TrackId id, ReceiveDemand demand, ControlOptions opts,
                               std::shared_ptr<Endpoint> expected = {}) {
    std::lock_guard controller_lock(controller_mutex);
    PendingOperation<void> operation;
    auto* pending = &operation;
    const auto submitted = SteadyClock::now();
    run_request(
        [&] {
          if (opts.timeout.count() < 0 || opts.timeout > std::chrono::minutes(5)) {
            pending->fail(
                detail::error("Invalid receive demand timeout", ErrorCode::invalid_argument));
            return;
          }
          if (opts.stop.stop_requested()) {
            pending->fail(error("Receive demand cancelled before dispatch", ErrorCode::cancelled));
            return;
          }
          const auto deadline =
              submitted + (opts.timeout.count() ? opts.timeout : options.operation_timeout);
          if (SteadyClock::now() >= deadline) {
            pending->fail(error("Receive demand expired before dispatch", ErrorCode::timeout));
            return;
          }
          const auto it = tracks.find({false, id});
          if (connection != ConnectionState::connected || it == tracks.end() ||
              (expected && expected != it->second.endpoint) ||
              it->second.endpoint->snapshot().state != TrackState::ready) {
            pending->fail(
                error("Subscription is not ready for receive demand", ErrorCode::stale_handle));
            return;
          }
          validate_receive_demand(demand, it->second.endpoint->descriptor.kind);
          if (controls.size() >= 128 || next_control == std::numeric_limits<uint32_t>::max()) {
            pending->fail(error("RTC control capacity exhausted", ErrorCode::resource_exhausted));
            return;
          }
          const auto endpoint = it->second.endpoint;
          const auto binding = endpoint->binding();
          const auto operation = ++next_control;
          if (!add_control(operation, PendingControl{pending, epoch, deadline, opts.stop,
                                                     [endpoint, binding, demand] {
                                                       if (endpoint->matches(binding))
                                                         endpoint->set_receive_demand(demand);
                                                     }})) {
            return;
          }
          try {
            transport->set_receive_demand({operation, binding, demand, deadline, opts.stop});
          } catch (...) {
            erase_control(operation);
            pending->fail(std::current_exception());
          }
        },
        operation);
    return wait(operation);
  }

  void finish_removal(const std::shared_ptr<Endpoint>& endpoint, std::exception_ptr failure = {}) {
    const auto it = std::find_if(removals.begin(), removals.end(),
                                 [&](const auto& removal) { return removal.endpoint == endpoint; });
    if (it == removals.end()) {
      return;  // The room stop barrier already settled this incarnation.
    }
    auto* waiting = it->waiting;
    removals.erase(it);
    if (waiting) {
      if (failure) {
        waiting->fail(failure);
      } else {
        waiting->succeed();
      }
    }
  }

  void finish_all_removals() {
    while (!removals.empty()) {
      finish_removal(removals.back().endpoint);
    }
  }

  void close_entry(std::map<Key, Entry>::iterator it, PendingOperation<void>* pending) {
    if (removals.size() >= 1024) {
      if (pending) {
        pending->fail(error("RTC detachment queue exhausted", ErrorCode::resource_exhausted));
      }
      return;
    }
    auto entry = it->second;
    erase_track(it);  // Remove intent before detachment or a late publication ACK.
    entry.endpoint->finish(TrackState::closed);
    if (entry.completion) {
      entry.completion(error("RTC track operation cancelled", ErrorCode::cancelled));
    }
    refresh();
    detach_endpoint(entry.endpoint, pending);
  }

  void detach_endpoint(const std::shared_ptr<Endpoint>& endpoint,
                       PendingOperation<void>* pending = {}) {
    if (removals.size() >= 1024) {
      if (pending) {
        pending->fail(error("RTC detachment queue exhausted", ErrorCode::resource_exhausted));
      }
      return;
    }
    removals.push_back({endpoint, pending});
    auto weak = weak_from_this();
    try {
      transport->remove(endpoint->binding(), endpoint->publication,
                        [weak, endpoint](std::exception_ptr failure) {
                          if (auto state = weak.lock()) {
                            std::lock_guard lock(state->mutex);
                            if (!state->shutting_down) {
                              state->completed_removals.emplace(endpoint, failure);
                              state->wakeup->signal();
                            }
                          }
                        });
    } catch (...) {
      finish_removal(endpoint, std::current_exception());
    }
  }

  void wait_removal(const std::shared_ptr<Endpoint>& endpoint, const TrackId& id, bool publication,
                    PendingOperation<void>* pending) {
    // An endpoint handle targets its exact incarnation. An ID-only repeated close
    // targets the most recent detachment of that ID, never an older incarnation.
    auto it = std::find_if(removals.rbegin(), removals.rend(), [&](const auto& removal) {
      return endpoint ? removal.endpoint == endpoint
                      : removal.endpoint->descriptor.id == id &&
                            removal.endpoint->publication == publication;
    });
    if (it == removals.rend()) {
      pending->succeed();
      return;
    }
    it->waiting = pending;
  }

  ErrorCode close_track(const std::shared_ptr<Endpoint>& endpoint) {
    std::lock_guard controller_lock(controller_mutex);
    PendingOperation<void> operation;
    auto* pending = &operation;
    run_request(
        [&] {
          const auto binding = endpoint->binding();
          auto it = tracks.find({binding.publication, binding.id});
          if (it != tracks.end() && it->second.endpoint == endpoint) {
            close_entry(it, pending);
          } else {
            wait_removal(endpoint, binding.id, binding.publication, pending);
          }
        },
        operation);
    return wait(operation);
  }

  ErrorCode close_id(const TrackId& id, bool publication) {
    std::lock_guard controller_lock(controller_mutex);
    PendingOperation<void> operation;
    auto* pending = &operation;
    run_request(
        [&] {
          auto it = tracks.find({publication, id});
          if (it != tracks.end()) {
            close_entry(it, pending);
          } else {
            wait_removal({}, id, publication, pending);
          }
        },
        operation);
    return wait(operation);
  }

  ErrorCode request_keyframe(const std::shared_ptr<Endpoint>& endpoint) {
    std::lock_guard controller_lock(controller_mutex);
    PendingOperation<void> operation;
    auto* pending = &operation;
    run_request(
        [&] {
          const auto binding = endpoint->binding();
          auto it = tracks.find({false, binding.id});
          if (it == tracks.end() || it->second.endpoint != endpoint ||
              endpoint->snapshot().state != TrackState::ready) {
            pending->fail(detail::error("RTC subscription is not ready", ErrorCode::stale_handle));
            return;
          }
          if (endpoint->descriptor.kind != MediaKind::video) {
            pending->fail(
                detail::error("Audio has no keyframe request", ErrorCode::invalid_argument));
            return;
          }
          endpoint->require_keyframe();
          send_pli(it->second);
          pending->succeed();
        },
        operation);
    return wait(operation);
  }

  void send_pli(Entry& entry) {
    const auto now = SteadyClock::now();
    if (now - entry.last_pli < std::chrono::milliseconds(250))
      return;
    transport->request_keyframe(entry.endpoint->binding());
    entry.last_pli = now;
  }

  void handle(TransportEvent event) {
    if (event.kind == TransportEventKind::terminal_error) {
      stop_all(ConnectionState::failed, event.error, event.failure);
      return;
    }
    if (event.kind == TransportEventKind::interrupted) {
      if (connection == ConnectionState::closed || connection == ConnectionState::failed ||
          connection == ConnectionState::reconnecting)
        return;
      connection = ConnectionState::reconnecting;
      interrupt_controls();
      ++epoch;
      for (auto& [key, entry] : tracks)
        entry.endpoint->rebind(epoch);
      refresh();
      emit({RoomEventKind::reconnecting, {}, {}, epoch, event.error});
      return;
    }
    if (event.kind == TransportEventKind::connected) {
      if (connection != ConnectionState::connecting && connection != ConnectionState::reconnecting)
        return;
      connection = ConnectionState::connected;
      for (auto it = tracks.begin(); it != tracks.end();) {
        const auto key = it->first;
        auto& entry = it->second;
        ++it;
        try {
          dispatch(entry);
        } catch (const std::exception& e) {
          fail_track(key, std::current_exception(), e.what());
        }
      }
      refresh();
      if (joining) {
        joining->succeed();
        joining = nullptr;
      }
      emit({RoomEventKind::connected, {}, {}, epoch, {}});
      return;
    }
    if (event.kind == TransportEventKind::control_completed) {
      auto it = controls.find(event.operation_id);
      if (it == controls.end() || it->second.epoch != event.binding.epoch)
        return;
      if (it->second.stop.stop_requested() || SteadyClock::now() >= it->second.deadline)
        it->second.pending->fail(error(
            "RTC control completion arrived after cancellation/deadline; remote outcome is unknown",
            ErrorCode::outcome_unknown));
      else if (event.control_error)
        it->second.pending->fail(
            std::make_exception_ptr(SDKError(ErrorDomain::signaling, *event.control_error,
                                             event.error, false, event.control_reason)));
      else {
        if (it->second.applied)
          it->second.applied();
        it->second.pending->succeed();
      }
      erase_control(event.operation_id);
      return;
    }
    if (event.kind == TransportEventKind::statistics) {
      if (event.binding.epoch != epoch || !event.statistics)
        return;
      RoomEvent notification{RoomEventKind::statistics, {}, {}, epoch, {}};
      notification.statistics = std::move(event.statistics);
      emit(std::move(notification));
      return;
    }
    if (event.kind == TransportEventKind::subscription_permission_changed) {
      if (event.binding.epoch != epoch || !event.permission ||
          connection == ConnectionState::closed || connection == ConnectionState::failed)
        return;
      RoomEvent notification{RoomEventKind::subscription_permission_changed, {}, {}, epoch, {}};
      notification.permission = std::move(event.permission);
      emit(std::move(notification));
      return;
    }
    if (event.kind == TransportEventKind::session_changed) {
      if (!event.session || event.binding.epoch != epoch || event.session->epoch != epoch ||
          connection == ConnectionState::closed || connection == ConnectionState::failed)
        return;
      {
        std::lock_guard lock(mutex);
        if (status.session && status.session->revision >= event.session->revision)
          return;
        status.session = std::move(event.session);
      }
      emit({RoomEventKind::session_changed, {}, {}, epoch, {}});
      return;
    }
    if (event.kind == TransportEventKind::remote_track_added ||
        event.kind == TransportEventKind::remote_track_removed) {
      if (event.binding.epoch != epoch || connection == ConnectionState::closed ||
          connection == ConnectionState::failed)
        return;
      emit({event.kind == TransportEventKind::remote_track_added
                ? RoomEventKind::remote_track_added
                : RoomEventKind::remote_track_removed,
            event.binding.id,
            std::move(event.descriptor),
            epoch,
            {}});
      return;
    }
    if (event.kind == TransportEventKind::encryption_state_changed) {
      if (event.binding.epoch == epoch) {
        RoomEvent notification{
            RoomEventKind::encryption_state_changed, event.binding.id, {}, epoch, {}};
        notification.encryption = std::move(event.encryption);
        emit(std::move(notification));
      }
      return;
    }
    auto it = tracks.find({event.binding.publication, event.binding.id});
    if (it == tracks.end() || !it->second.endpoint->matches(event.binding)) {
      if (event.kind == TransportEventKind::published ||
          event.kind == TransportEventKind::subscribed)
        transport->remove(
            event.binding,
            event.binding.publication);  // compensate a cancelled/timed-out publication
      return;
    }
    auto& entry = it->second;
    auto& endpoint = *entry.endpoint;
    switch (event.kind) {
      case TransportEventKind::mute_changed:
        if (endpoint.publication) {
          endpoint.set_muted(event.muted);
          emit_track({RoomEventKind::track_mute_changed, event.binding.id, {}, epoch, {}}, true);
        }
        break;
      case TransportEventKind::published:
      case TransportEventKind::subscribed:
        if (connection != ConnectionState::connected)
          return;
        endpoint.ready(std::move(event.server_sid));
        if (entry.completion) {
          entry.completion({});
          entry.completion = {};
        }
        emit_track({RoomEventKind::track_ready, event.binding.id, endpoint.descriptor, epoch, {}},
                   event.binding.publication);
        break;
      case TransportEventKind::operation_failed:
      case TransportEventKind::removed: {
        const auto failure = normalized_failure(
            event.failure ? event.failure : error(event.error, ErrorCode::request_failed),
            {"track", event.binding.id, {}, epoch});
        const auto info = describe_failure(failure);
        const bool failed = event.kind == TransportEventKind::operation_failed;
        endpoint.finish(failed ? TrackState::failed : TrackState::closed, event.error,
                        failed ? info : nullptr);
        if (entry.completion) {
          entry.completion(failure);
        }
        RoomEvent notification{failed ? RoomEventKind::track_failed : RoomEventKind::track_removed,
                               event.binding.id, endpoint.descriptor, epoch, event.error};
        if (failed) {
          notification.failure = info;
        }
        emit_track(std::move(notification), event.binding.publication);
        erase_track(it);
        refresh();
        break;
      }
      case TransportEventKind::frame:
        if (!endpoint.publication && event.frame)
          endpoint.write(std::move(*event.frame));
        break;
      case TransportEventKind::feedback:
        if (endpoint.publication && event.feedback)
          endpoint.update_feedback(*event.feedback);
        break;
      case TransportEventKind::writable:
        ready_work->mark({WorkKind::track, event.binding.incarnation});
        break;
      default:
        break;
    }
  }

  void service_track(uint64_t incarnation) {
    const auto found = track_keys.find(incarnation);
    if (found == track_keys.end()) {
      return;
    }
    const auto key = found->second;
    auto& entry = tracks.at(key);
    ++tracks_serviced;
    const auto now = SteadyClock::now();
    if (entry.completion && now >= entry.deadline) {
      fail_track(key, error("RTC track operation timed out", ErrorCode::timeout),
                 "RTC track operation timed out");
      return;
    }
    const WorkKey work{WorkKind::track, incarnation};
    try {
      std::optional<SteadyClock::time_point> next;
      const auto include = [&](std::optional<SteadyClock::time_point> time) {
        if (time && (!next || *time < *next)) {
          next = time;
        }
      };
      if (entry.completion) {
        include(entry.deadline);
      }
      if (connection == ConnectionState::connected &&
          entry.endpoint->snapshot().state == TrackState::ready) {
        if (entry.endpoint->publication) {
          // A bounded quantum for each notified track. Would-block sleeps until
          // the native sender explicitly reports writable; other tracks proceed.
          bool blocked = false;
          for (uint32_t n = 0; n < entry.endpoint->descriptor.priority; ++n) {
            auto item = entry.endpoint->front();
            if (!item) {
              break;
            }
            const auto sent = transport->send(entry.endpoint->binding(), item->frame);
            if (sent == SendResult::would_block) {
              blocked = true;
              break;
            }
            entry.endpoint->consumed(item->serial, sent == SendResult::dropped);
          }
          if (!blocked && entry.endpoint->front()) {
            ready_work->mark(work);
          }
        }
        // Expiring queued video may request a keyframe; do it before testing PLI.
        include(entry.endpoint->next_expiration());
        if (!entry.endpoint->publication && entry.endpoint->descriptor.kind == MediaKind::video) {
          const auto feedback = entry.endpoint->feedback();
          if (std::any_of(feedback.encodings.begin(), feedback.encodings.end(),
                          [](const auto& value) {
                            return value.second.keyframe_required && value.second.active;
                          })) {
            send_pli(entry);
            include(entry.last_pli + std::chrono::milliseconds(250));
          }
        }
      }
      deadlines.set(work, next);
    } catch (const std::exception& e) {
      fail_track(key, std::current_exception(), e.what());
    }
  }

  void service_ready() {
    const auto now = SteadyClock::now();
    if (joining && now >= join_deadline) {
      stop_all(ConnectionState::failed, "RTC join timed out",
               error("RTC join timed out", ErrorCode::timeout));
      return;
    }
    auto work = ready_work->take();
    for (const auto& expired : deadlines.take_due(now)) {
      work.insert(expired);
    }
    for (const auto& [kind, identity] : work) {
      if (kind == WorkKind::track) {
        service_track(identity);
      } else {
        const auto id = static_cast<uint32_t>(identity);
        const auto found = controls.find(id);
        if (found != controls.end() &&
            (found->second.stop.stop_requested() || now >= found->second.deadline)) {
          auto pending = found->second.pending;
          erase_control(id);
          pending->fail(
              error("RTC control cancelled or timed out after dispatch; remote outcome is unknown",
                    ErrorCode::outcome_unknown));
        }
      }
    }
    refresh();
  }

  std::optional<SteadyClock::time_point> next_deadline() const {
    auto next = deadlines.next();
    if (joining && (!next || join_deadline < *next)) {
      next = join_deadline;
    }
    return next;
  }

  void pump() noexcept {
    ++controller_wakeups;
    std::map<std::shared_ptr<Endpoint>, std::exception_ptr> failures, detached;
    {
      std::lock_guard lock(mutex);
      failures.swap(graph_failures);
      detached.swap(completed_removals);
    }
    try {
      handle_service_failure();
      for (const auto& [endpoint, failure] : detached) {
        finish_removal(endpoint, failure);
      }
      for (const auto& [endpoint, failure] : failures) {
        const Key key{endpoint->publication, endpoint->descriptor.id};
        const auto it = tracks.find(key);
        if (it != tracks.end() && it->second.endpoint == endpoint) {
          fail_track(key, failure, describe_failure(failure)->message);
        }
      }
    } catch (...) {
      stop_all(ConnectionState::failed, "Graph failure cleanup failed", std::current_exception());
    }
    try {
      if (connection == ConnectionState::connecting || connection == ConnectionState::connected ||
          connection == ConnectionState::reconnecting) {
        for (auto& event : transport->take_events()) {
          if (connection == ConnectionState::closed || connection == ConnectionState::failed) {
            break;
          }
          handle(std::move(event));
        }
        service_ready();
      }
    } catch (const std::exception& e) {
      stop_all(ConnectionState::failed, e.what(), std::current_exception());
    } catch (...) {
      stop_all(ConnectionState::failed, "Unknown RTC transport failure");
    }
  }

  void run(std::stop_token stop) noexcept {
    while (!stop.stop_requested()) {
      const auto observed = wakeup->revision();
      std::optional<SteadyClock::time_point> deadline;
      {
        std::lock_guard controller_lock(controller_mutex);
        pump();
        deadline = next_deadline();
      }
      wakeup->wait(observed, stop, deadline);
    }
    std::lock_guard controller_lock(controller_mutex);
    stop_all(ConnectionState::closed, "RTC room destroyed");
  }
};
}  // namespace detail

PublishedTrack::PublishedTrack(std::shared_ptr<detail::Endpoint> endpoint,
                               std::weak_ptr<detail::RoomEngineState> session)
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
      detail::OperationGuard guard(*session->control);
      return session->set_muted(endpoint_->descriptor.id, muted, options, endpoint_);
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
      detail::OperationGuard guard(*session->control);
      return close_locked();
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
                                 std::weak_ptr<detail::RoomEngineState> session)
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
      detail::OperationGuard guard(*session->control);
      return session->set_receive_demand(endpoint_->descriptor.id, demand, options, endpoint_);
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
      detail::OperationGuard guard(*session->control);
      return close_locked();
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

RoomEngine::RoomEngine(TransportFactory factory)
    : RoomEngine(std::move(factory), std::make_shared<detail::SessionControl>()) {}

RoomEngine::RoomEngine(TransportFactory factory, std::shared_ptr<detail::SessionControl> control)
    : state_(std::make_shared<detail::RoomEngineState>(std::move(factory), std::move(control))) {
  auto* state = state_.get();
  state_->control->async = std::make_shared<detail::AsyncControl>(
      *state_->control, [state](auto task) { state->post_notification(std::move(task), true); },
      [wakeup = state_->wakeup] { wakeup->signal(); });
  state_->set_handler([state](const RoomEvent& event) {
    if (!event.publication) {
      if (auto observer = state->room_observer.get().lock()) {
        observer->on_room_event(event);
      }
    } else if (*event.publication) {
      if (auto observer = state->publication_observer.get().lock()) {
        observer->on_publication_event(event);
      }
    } else if (auto observer = state->subscription_observer.get().lock()) {
      observer->on_subscription_event(event);
    }
  });
}

std::shared_ptr<detail::SessionControl> RoomEngine::control() const {
  return state_->control;
}

void SubscribedTrack::request_keyframe_async() {
  endpoint_->require_keyframe();
}

RoomEngine::~RoomEngine() {
  state_->control->async->shutdown();
  state_->set_handler({});
  state_->shutdown();
}

Result<std::shared_ptr<RoomServices>> RoomEngine::services() const {
  return detail::control_boundary([&] {
    std::lock_guard lock(state_->mutex);
    if (!state_->services) {
      return detail::failure_result<std::shared_ptr<RoomServices>>(
          ErrorCode::unsupported, "This transport does not provide room services");
    }
    return Result<std::shared_ptr<RoomServices>>::success(state_->services);
  });
}

ErrorCode RoomEngine::join(RoomOptions options) {
  return detail::control_boundary([&] {
    return state_->control->async->submit(
        "room.join",
        [this, options = std::move(options)]() mutable {
          detail::OperationGuard guard(*state_->control);
          return join_locked(std::move(options));
        },
        state_->room_observer.get(),
        [](auto& observer, auto result) { observer.on_join_completed(std::move(result)); });
  });
}

ErrorCode RoomEngine::join_locked(RoomOptions options) {
  std::lock_guard controller_lock(state_->controller_mutex);
  detail::PendingOperation<void> operation;
  auto* pending = &operation;
  auto* s = state_.get();
  s->run_request(
      [&] {
        if (!s->transport) {
          pending->fail(s->initialization_failure ? s->initialization_failure
                                                  : detail::error("RTC room requires a transport",
                                                                  ErrorCode::invalid_argument));
          return;
        }
        if (s->connection != ConnectionState::disconnected &&
            s->connection != ConnectionState::closed && s->connection != ConnectionState::failed) {
          {
            pending->fail(
                detail::error("Leave the RTC room before joining again", ErrorCode::invalid_state));
            return;
          }
        }
        if (!options.max_queued_bytes || options.max_queued_bytes > 1024ull * 1024 * 1024 ||
            !options.max_tracks || options.max_tracks > 1024 || !options.max_events ||
            options.max_events > 65536 ||
            options.operation_timeout <= std::chrono::milliseconds::zero() ||
            options.operation_timeout > std::chrono::minutes(5) ||
            (options.stats_interval.count() != 0 &&
             (options.stats_interval < std::chrono::milliseconds(200) ||
              options.stats_interval > std::chrono::minutes(1)))) {
          {
            pending->fail(detail::error("Invalid RTC room options", ErrorCode::invalid_argument));
            return;
          }
        }
        if (options.subscription_permissions)
          detail::validate_subscription_permissions(*options.subscription_permissions);
        if (s->connection != ConnectionState::disconnected) {
          auto replacement = s->transport_factory();
          if (!replacement) {
            {
              pending->fail(detail::error("RTC transport factory returned null",
                                          ErrorCode::invalid_argument));
              return;
            }
          }
          s->attach_service_notifications(*replacement);
          auto services = replacement->services();
          s->transport = std::move(replacement);
          std::lock_guard lock(s->mutex);
          // Release old service state after unlocking; it may own executors.
          s->services.swap(services);
        }
        {
          std::lock_guard lock(s->mutex);
          s->events.clear();
          s->status = {};
        }
        s->options = options;
        s->budget = std::make_shared<detail::QueueBudget>(options.max_queued_bytes);
        s->connection = ConnectionState::connecting;
        ++s->epoch;
        s->join_deadline = SteadyClock::now() + options.operation_timeout;
        s->refresh();
        s->joining = pending;
        try {
          s->transport->attach_wakeup(s->wakeup);
          s->transport->set_queue_budget(s->budget);
          s->transport->start(options, s->epoch);
        } catch (...) {
          s->stop_all(ConnectionState::failed, "RTC transport start failed",
                      std::current_exception());
          pending->fail(std::current_exception());
        }
      },
      operation);
  return state_->wait(operation);
}

ErrorCode RoomEngine::publish(TrackDescriptor descriptor) {
  const auto track_id = descriptor.id;
  return detail::control_boundary([&] {
    return state_->control->async->submit(
        "publication.publish" + std::string(":") + track_id,
        [this, descriptor = std::move(descriptor)]() mutable {
          detail::OperationGuard guard(*state_->control);
          return publish_locked(std::move(descriptor));
        },
        state_->publication_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_publish_completed(track_id, std::move(result));
        });
  });
}

Result<std::shared_ptr<PublishedTrack>> RoomEngine::publish_locked(TrackDescriptor descriptor) {
  std::lock_guard controller_lock(state_->controller_mutex);
  detail::PendingOperation<std::shared_ptr<PublishedTrack>> operation;
  auto* pending = &operation;
  auto* s = state_.get();
  s->run_request(
      [&] {
        auto endpoint = s->make_endpoint(std::move(descriptor), true);
        if (!endpoint) {
          pending->fail(
              detail::error("Join the RTC room before adding tracks", ErrorCode::invalid_state));
          return;
        }
        auto track = std::shared_ptr<PublishedTrack>(new PublishedTrack(endpoint, state_));
        s->add(endpoint, [s, pending, track](auto failure) {
          if (failure)
            pending->fail(failure);
          else
            pending->succeed(track);
        });
      },
      operation);
  return state_->wait(operation);
}

ErrorCode RoomEngine::subscribe(TrackDescriptor descriptor) {
  const auto track_id = descriptor.id;
  return detail::control_boundary([&] {
    return state_->control->async->submit(
        "subscription.subscribe" + std::string(":") + track_id,
        [this, descriptor = std::move(descriptor)]() mutable {
          detail::OperationGuard guard(*state_->control);
          return subscribe_locked(std::move(descriptor));
        },
        state_->subscription_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_subscribe_completed(track_id, std::move(result));
        });
  });
}

Result<std::shared_ptr<SubscribedTrack>> RoomEngine::subscribe_locked(TrackDescriptor descriptor) {
  std::lock_guard controller_lock(state_->controller_mutex);
  detail::PendingOperation<std::shared_ptr<SubscribedTrack>> operation;
  auto* pending = &operation;
  auto* s = state_.get();
  s->run_request(
      [&] {
        auto endpoint = s->make_endpoint(std::move(descriptor), false);
        if (!endpoint) {
          pending->fail(
              detail::error("Join the RTC room before adding tracks", ErrorCode::invalid_state));
          return;
        }
        auto track = std::shared_ptr<SubscribedTrack>(new SubscribedTrack(endpoint, state_));
        s->add(endpoint, [s, pending, track](auto failure) {
          if (failure)
            pending->fail(failure);
          else
            pending->succeed(track);
        });
      },
      operation);
  return state_->wait(operation);
}

ErrorCode RoomEngine::leave() {
  return detail::control_boundary([&] {
    return state_->control->async->submit(
        "room.leave",
        [this]() mutable {
          detail::OperationGuard guard(*state_->control);
          return leave_locked();
        },
        state_->room_observer.get(),
        [](auto& observer, auto result) { observer.on_leave_completed(std::move(result)); }, true);
  });
}

ErrorCode RoomEngine::leave_locked() {
  std::lock_guard controller_lock(state_->controller_mutex);
  detail::PendingOperation<void> operation;
  auto* pending = &operation;
  auto* s = state_.get();
  s->run_request(
      [&] {
        if (s->connection != ConnectionState::closed)
          s->stop_all(ConnectionState::closed, "RTC room left");
        pending->succeed();
      },
      operation);
  auto result = state_->wait(operation);
  {
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(
        lock, [&] { return !state_->notification_scheduled && state_->extra_notifications == 0; });
  }
  return result;
}

ErrorCode RoomEngine::unpublish(const TrackId& id) {
  const auto track_id = id;
  return detail::control_boundary([&] {
    return state_->control->async->submit(
        "publication.remove" + std::string(":") + track_id,
        [this, id]() mutable {
          detail::OperationGuard guard(*state_->control);
          return unpublish_locked(id);
        },
        state_->publication_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_unpublish_completed(track_id, std::move(result));
        });
  });
}

ErrorCode RoomEngine::unpublish_locked(const TrackId& id) {
  return state_->close_id(id, true);
}

ErrorCode RoomEngine::unsubscribe(const TrackId& id) {
  const auto track_id = id;
  return detail::control_boundary([&] {
    return state_->control->async->submit(
        "subscription.remove" + std::string(":") + track_id,
        [this, id]() mutable {
          detail::OperationGuard guard(*state_->control);
          return unsubscribe_locked(id);
        },
        state_->subscription_observer.get(),
        [track_id](auto& observer, auto result) {
          observer.on_unsubscribe_completed(track_id, std::move(result));
        });
  });
}

ErrorCode RoomEngine::unsubscribe_locked(const TrackId& id) {
  return state_->close_id(id, false);
}

ErrorCode RoomEngine::set_muted(const TrackId& id, bool muted, ControlOptions options) {
  const auto track_id = id;
  return detail::control_boundary([&] {
    return state_->control->async->submit(
        "publication.mute" + std::string(":") + track_id,
        [this, id, muted, options]() mutable {
          detail::OperationGuard guard(*state_->control);
          return set_muted_locked(id, muted, options);
        },
        state_->publication_observer.get(),
        [track_id, muted](auto& observer, auto result) {
          observer.on_muted_completed(track_id, muted, std::move(result));
        });
  });
}

ErrorCode RoomEngine::set_muted_locked(const TrackId& id, bool muted, ControlOptions options) {
  return state_->set_muted(id, muted, options);
}

ErrorCode RoomEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                         ControlOptions options) {
  const auto track_id = id;
  return detail::control_boundary([&] {
    return state_->control->async->submit(
        "subscription.demand" + std::string(":") + track_id,
        [this, id, demand, options]() mutable {
          detail::OperationGuard guard(*state_->control);
          return set_receive_demand_locked(id, demand, options);
        },
        state_->subscription_observer.get(),
        [track_id, demand](auto& observer, auto result) {
          observer.on_receive_demand_completed(track_id, demand, std::move(result));
        });
  });
}

ErrorCode RoomEngine::set_receive_demand_locked(const TrackId& id, ReceiveDemand demand,
                                                ControlOptions options) {
  return state_->set_receive_demand(id, demand, options);
}

ErrorCode RoomEngine::update_local_participant(ParticipantUpdate update, ControlOptions options) {
  return detail::control_boundary([&] {
    return state_->control->async->submit(
        "room.participant",
        [this, update = std::move(update), options]() mutable {
          detail::OperationGuard guard(*state_->control);
          return update_local_participant_locked(std::move(update), options);
        },
        state_->room_observer.get(),
        [](auto& observer, auto result) {
          observer.on_participant_update_completed(std::move(result));
        });
  });
}

ErrorCode RoomEngine::update_local_participant_locked(ParticipantUpdate update,
                                                      ControlOptions options) {
  std::lock_guard controller_lock(state_->controller_mutex);
  detail::PendingOperation<void> operation;
  auto* pending = &operation;
  try {
    size_t bytes = update.name.size() + update.metadata.size();
    if (update.name.size() > 1024 || update.metadata.size() > 65536 ||
        update.attributes.size() > 128 || !livekit::sdk::detail::valid_utf8(update.name) ||
        !livekit::sdk::detail::valid_utf8(update.metadata) || options.timeout.count() < 0 ||
        options.timeout > std::chrono::minutes(5)) {
      return ErrorCode::invalid_argument;
    }
    for (const auto& [key, value] : update.attributes) {
      if (key.empty() || key.size() > 256 || value.size() > 16384 ||
          !livekit::sdk::detail::valid_utf8(key) || !livekit::sdk::detail::valid_utf8(value)) {
        return ErrorCode::invalid_argument;
      }
      bytes += key.size() + value.size();
    }
    if (bytes > 65536) {
      return ErrorCode::invalid_argument;
    }
    auto* s = state_.get();
    const auto submitted = SteadyClock::now();
    s->run_request(
        [&] {
          if (s->connection != ConnectionState::connected) {
            pending->fail(detail::error("Room is not connected"));
            return;
          }
          if (!s->transport->capabilities().participant_update_ack) {
            {
              pending->fail(
                  detail::error("Backend does not support acknowledged participant updates",
                                ErrorCode::unsupported));
              return;
            }
          }
          if (options.stop.stop_requested()) {
            pending->fail(detail::error("Participant update cancelled before dispatch",
                                        ErrorCode::cancelled));
            return;
          }
          const auto deadline =
              submitted +
              (options.timeout.count() ? options.timeout : s->options.operation_timeout);
          if (SteadyClock::now() >= deadline) {
            pending->fail(
                detail::error("Participant update expired before dispatch", ErrorCode::timeout));
            return;
          }
          if (s->controls.size() >= 128 ||
              s->next_control == std::numeric_limits<uint32_t>::max()) {
            pending->fail(detail::error("Participant update capacity exhausted",
                                        ErrorCode::resource_exhausted));
            return;
          }
          const auto id = ++s->next_control;
          if (!s->add_control(id, detail::RoomEngineState::PendingControl{
                                      pending, s->epoch, deadline, options.stop})) {
            return;
          }
          try {
            s->transport->update_participant(
                {id, s->epoch, std::move(update), deadline, options.stop});
          } catch (...) {
            s->erase_control(id);
            pending->fail(std::current_exception());
          }
        },
        operation);
  } catch (...) {
    pending->fail(std::current_exception());
  }
  return state_->wait(operation);
}

ErrorCode RoomEngine::set_subscription_permissions(SubscriptionPermissions policy,
                                                   ControlOptions options) {
  return detail::control_boundary([&] {
    return state_->control->async->submit(
        "room.permissions",
        [this, policy = std::move(policy), options]() mutable {
          detail::OperationGuard guard(*state_->control);
          return set_subscription_permissions_locked(std::move(policy), options);
        },
        state_->room_observer.get(),
        [](auto& observer, auto result) {
          observer.on_subscription_permissions_completed(std::move(result));
        });
  });
}

ErrorCode RoomEngine::set_subscription_permissions_locked(SubscriptionPermissions policy,
                                                          ControlOptions options) {
  std::lock_guard controller_lock(state_->controller_mutex);
  detail::PendingOperation<void> operation;
  auto* pending = &operation;
  try {
    detail::validate_subscription_permissions(policy);
    if (options.timeout.count() < 0 || options.timeout > std::chrono::minutes(5)) {
      return ErrorCode::invalid_argument;
    }
    auto* s = state_.get();
    const auto submitted = SteadyClock::now();
    s->run_request(
        [&] {
          if (s->connection != ConnectionState::connected) {
            pending->fail(detail::error("Room is not connected"));
            return;
          }
          if (options.stop.stop_requested()) {
            pending->fail(
                detail::error("Permission update cancelled before dispatch", ErrorCode::cancelled));
            return;
          }
          const auto deadline =
              submitted +
              (options.timeout.count() ? options.timeout : s->options.operation_timeout);
          if (SteadyClock::now() >= deadline) {
            pending->fail(
                detail::error("Permission update expired before dispatch", ErrorCode::timeout));
            return;
          }
          if (s->controls.size() >= 128 ||
              s->next_control == std::numeric_limits<uint32_t>::max()) {
            pending->fail(
                detail::error("RTC control capacity exhausted", ErrorCode::resource_exhausted));
            return;
          }
          const auto id = ++s->next_control;
          if (!s->add_control(id, detail::RoomEngineState::PendingControl{
                                      pending, s->epoch, deadline, options.stop})) {
            return;
          }
          try {
            s->transport->set_subscription_permissions(
                {id, s->epoch, std::move(policy), deadline, options.stop});
          } catch (...) {
            s->erase_control(id);
            pending->fail(std::current_exception());
          }
        },
        operation);
  } catch (...) {
    pending->fail(std::current_exception());
  }
  return state_->wait(operation);
}

RoomSnapshot RoomEngine::snapshot() const {
  std::lock_guard lock(state_->mutex);
  return state_->status;
}

void RoomEngine::set_room_observer(std::weak_ptr<RoomObserver> observer) {
  state_->room_observer.set(std::move(observer));
}

void RoomEngine::set_publication_observer(std::weak_ptr<PublicationObserver> observer) {
  state_->publication_observer.set(std::move(observer));
}

void RoomEngine::set_subscription_observer(std::weak_ptr<SubscriptionObserver> observer) {
  state_->subscription_observer.set(std::move(observer));
}

ErrorCode RoomEngine::close() {
  return detail::control_boundary([&] {
    return state_->control->async->submit(
        "room.close",
        [this] {
          detail::OperationGuard guard(*state_->control);
          state_->control->closed = true;
          return leave_locked();
        },
        state_->room_observer.get(),
        [](auto& observer, auto result) { observer.on_close_completed(std::move(result)); }, true,
        true);
  });
}

void RoomEngine::set_event_handler_locked(std::function<void(const RoomEvent&)> handler) {
  state_->set_handler(std::move(handler));
}

void RoomEngine::notification_failed() {
  state_->notification_failed();
}

void RoomEngine::post_notification(std::function<void()> callback) {
  state_->post_notification(std::move(callback));
}

}  // namespace livekit::sdk::rtc
