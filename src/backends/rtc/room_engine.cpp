#include "livekit/rtc/room_engine.hpp"
#include "backends/rtc/endpoint.hpp"
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

template <class T>
AsyncResult<T> rejected(std::exception_ptr failure) {
  CompletionSource<T> result;
  result.set_exception(failure);
  return result.get_result();
}
}  // namespace

struct RoomEngineState : std::enable_shared_from_this<RoomEngineState> {
  explicit RoomEngineState(RoomEngine::TransportFactory factory)
      : transport_factory(std::move(factory)) {
    if (!transport_factory) {
      throw std::invalid_argument("RTC room requires a transport factory");
    }
    transport = transport_factory();
    if (!transport)
      throw std::invalid_argument("RTC room requires a transport");
    transport->attach_wakeup(wakeup);
    services = transport->services();
    notifications = vi::TaskQueue::create("rtc.room.notifications");
    notification_dispatch.bind(*notifications);
    attach_service_notifications(*transport);
    worker = std::jthread([this](std::stop_token stop) { run(stop); });
  }

  ~RoomEngineState() {
    set_handler({});
    shutdown();
    notification_dispatch.close();
    notifications.reset();
  }

  struct Command {
    std::function<void()> run;
    std::function<void(std::exception_ptr)> cancel;
  };

  struct Entry {
    std::shared_ptr<Endpoint> endpoint;
    SteadyClock::time_point deadline;
    std::function<void(std::exception_ptr)> completion;
    SteadyClock::time_point last_pli{};
  };

  struct Removal {
    std::shared_ptr<Endpoint> endpoint;
    std::vector<std::shared_ptr<CompletionSource<void>>> waiters;
  };

  using Key = std::pair<bool, TrackId>;
  enum class WorkKind { track, control };
  using WorkKey = std::pair<WorkKind, uint64_t>;
  RoomEngine::TransportFactory transport_factory;
  std::unique_ptr<RoomTransport> transport;
  std::shared_ptr<RoomServices> services;
  std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup =
      std::make_shared<livekit::sdk::detail::WakeEvent>();
  std::shared_ptr<livekit::sdk::detail::ReadySet<WorkKey>> ready_work =
      std::make_shared<livekit::sdk::detail::ReadySet<WorkKey>>(wakeup, 1024 + 128);
  std::jthread worker;
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  std::deque<Command> commands;
  // At most one report per registered graph. Separate admission ensures a full
  // user command queue cannot suppress terminal graph failure notifications.
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
    adapter.attach_notifications(notification_dispatch, [failure = service_failure,
                                                         event = wakeup](std::exception_ptr cause) {
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
    if (error && connection != ConnectionState::closed && connection != ConnectionState::failed) {
      stop_all(ConnectionState::failed, describe_failure(error)->message, error);
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
      post({[this] {
              stop_all(ConnectionState::failed, "RTC event callback threw; handler disabled");
            },
            [](auto) {}});
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
              callback(*event);
            } catch (...) {
              notification_failed();
              break;
            }
          }
          std::lock_guard lock(mutex);
          notification_scheduled = false;
          schedule_events_locked();
        } catch (...) {
          {
            std::lock_guard lock(mutex);
            notification_scheduled = false;
          }
          notification_failed();
        }
      });
    } catch (...) {
      notification_scheduled = false;
      throw;
    }
  }

  void set_handler(std::function<void(const RoomEvent&)> handler) {
    {
      std::lock_guard lock(mutex);
      event_handler = std::move(handler);
      schedule_events_locked();
    }
    notifications->postSyncTask([] {});
  }

  void post_notification(std::function<void()> callback) {
    {
      std::lock_guard lock(mutex);
      if (extra_notifications >= 256) {
        throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                       "RTC notification task queue exhausted");
      }
      ++extra_notifications;
    }
    try {
      notifications->postTask([this, callback = std::move(callback)] {
        try {
          callback();
        } catch (...) {
          notification_failed();
        }
        std::lock_guard lock(mutex);
        --extra_notifications;
      });
    } catch (...) {
      std::lock_guard lock(mutex);
      --extra_notifications;
      throw;
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
  size_t removal_waiters = 0;
  std::shared_ptr<CompletionSource<void>> joining;
  SteadyClock::time_point join_deadline{};

  struct PendingControl {
    std::shared_ptr<CompletionSource<void>> promise;
    Epoch epoch;
    SteadyClock::time_point deadline;
    std::stop_token stop;
    std::function<void()> applied;
    std::unique_ptr<std::stop_callback<std::function<void()>>> cancellation;
  };

  uint32_t next_control = 0;
  std::map<uint32_t, PendingControl> controls;

  void add_control(uint32_t id, PendingControl control) {
    const WorkKey key{WorkKind::control, id};
    auto signal = ready_work->watch(key);
    try {
      control.cancellation = std::make_unique<std::stop_callback<std::function<void()>>>(
          control.stop, [signal] { signal->signal(); });
      deadlines.set(key, control.deadline);
      controls.emplace(id, std::move(control));
    } catch (...) {
      deadlines.erase(key);
      ready_work->forget(key);
      throw;
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
      pending.promise->set_exception(
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

  bool post(Command command) {
    std::exception_ptr failure;
    {
      std::lock_guard lock(mutex);
      if (shutting_down)
        failure = error("RTC room was destroyed");
      else if (commands.size() >= 1024)
        failure = error("RTC control queue exhausted", ErrorCode::resource_exhausted);
      else {
        commands.push_back(std::move(command));
        wakeup->signal();
        return true;
      }
    }
    command.cancel(failure);
    return false;
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
    status.capabilities = transport->capabilities();
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

  void emit(RoomEvent event) {
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
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                     "RTC room event queue exhausted");
    }
    if (result != EventAdmission::queued) {
      return;
    }
    schedule_events_locked();
    changed.notify_all();
  }

  void stop_all(ConnectionState state, const std::string& message,
                std::exception_ptr cause = {}) noexcept {
    connection = state;
    interrupt_controls();
    const auto failure =
        normalized_failure(cause ? cause : error(message), {"room", {}, {}, epoch});
    const auto info = describe_failure(failure);
    auto joining_result = std::move(joining);
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
    transport->stop();
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
      joining_result->set_exception(failure);
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
    emit(std::move(notification));
    refresh();
  }

  void add(std::shared_ptr<Endpoint> endpoint, std::function<void(std::exception_ptr)> completion) {
    if (connection != ConnectionState::connecting && connection != ConnectionState::connected &&
        connection != ConnectionState::reconnecting)
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed,
                     "Join the RTC room before adding tracks");
    const Key key{endpoint->publication, endpoint->descriptor.id};
    if (tracks.contains(key))
      throw std::invalid_argument("Duplicate RTC track id");
    if (tracks.size() >= options.max_tracks)
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                     "RTC room track limit exceeded");
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
    } catch (...) {
      erase_track(it);
      throw;
    }
    refresh();
  }

  std::shared_ptr<Endpoint> make_endpoint(TrackDescriptor descriptor, bool publication) {
    if (!budget)
      throw std::logic_error("Join the RTC room before adding tracks");
    validate_descriptor(descriptor, budget->limit, publication);
    BindingToken binding{descriptor.id, epoch, ++next_incarnation, publication};
    auto endpoint = std::make_shared<Endpoint>(std::move(descriptor), std::move(binding), budget);
    return endpoint;
  }

  AsyncResult<void> set_muted(TrackId id, bool muted, ControlOptions opts,
                              std::shared_ptr<Endpoint> expected = {}) {
    auto promise = std::make_shared<CompletionSource<void>>();
    auto result = promise->get_result();
    const auto submitted = SteadyClock::now();
    post({[this, id = std::move(id), muted, opts, expected, promise, submitted] {
            if (opts.timeout.count() < 0 || opts.timeout > std::chrono::minutes(5))
              throw std::invalid_argument("Invalid mute timeout");
            if (opts.stop.stop_requested())
              std::rethrow_exception(error("Mute cancelled before dispatch", ErrorCode::cancelled));
            const auto deadline =
                submitted + (opts.timeout.count() ? opts.timeout : options.operation_timeout);
            if (SteadyClock::now() >= deadline)
              std::rethrow_exception(error("Mute expired before dispatch", ErrorCode::timeout));
            const auto it = tracks.find({true, id});
            if (connection != ConnectionState::connected || it == tracks.end() ||
                (expected && expected != it->second.endpoint) ||
                it->second.endpoint->snapshot().state != TrackState::ready)
              std::rethrow_exception(
                  error("Publication is not ready for mute control", ErrorCode::stale_handle));
            if (controls.size() >= 128 || next_control == std::numeric_limits<uint32_t>::max())
              std::rethrow_exception(
                  error("RTC control capacity exhausted", ErrorCode::resource_exhausted));
            const auto endpoint = it->second.endpoint;
            const auto previous = endpoint->snapshot().muted;
            endpoint->set_muted(muted);  // Gate application admission before native dispatch.
            const auto operation = ++next_control;
            try {
              // Reserve the notification before dispatch. If it cannot be queued,
              // no native operation or pending promise may survive command rejection.
              emit({RoomEventKind::track_mute_changed, id, {}, epoch, {}});
              add_control(operation, PendingControl{promise, epoch, deadline, opts.stop});
              transport->set_muted({operation, endpoint->binding(), muted, deadline, opts.stop});
            } catch (...) {
              erase_control(operation);
              endpoint->set_muted(previous);
              throw;
            }
          },
          [promise](auto failure) { promise->set_exception(failure); }});
    return result;
  }

  AsyncResult<void> set_receive_demand(TrackId id, ReceiveDemand demand, ControlOptions opts,
                                       std::shared_ptr<Endpoint> expected = {}) {
    auto promise = std::make_shared<CompletionSource<void>>();
    auto result = promise->get_result();
    const auto submitted = SteadyClock::now();
    post({[this, id = std::move(id), demand, opts, expected, promise, submitted] {
            if (opts.timeout.count() < 0 || opts.timeout > std::chrono::minutes(5))
              throw std::invalid_argument("Invalid receive demand timeout");
            if (opts.stop.stop_requested())
              std::rethrow_exception(
                  error("Receive demand cancelled before dispatch", ErrorCode::cancelled));
            const auto deadline =
                submitted + (opts.timeout.count() ? opts.timeout : options.operation_timeout);
            if (SteadyClock::now() >= deadline)
              std::rethrow_exception(
                  error("Receive demand expired before dispatch", ErrorCode::timeout));
            const auto it = tracks.find({false, id});
            if (connection != ConnectionState::connected || it == tracks.end() ||
                (expected && expected != it->second.endpoint) ||
                it->second.endpoint->snapshot().state != TrackState::ready)
              std::rethrow_exception(
                  error("Subscription is not ready for receive demand", ErrorCode::stale_handle));
            validate_receive_demand(demand, it->second.endpoint->descriptor.kind);
            if (controls.size() >= 128 || next_control == std::numeric_limits<uint32_t>::max())
              std::rethrow_exception(
                  error("RTC control capacity exhausted", ErrorCode::resource_exhausted));
            const auto endpoint = it->second.endpoint;
            const auto binding = endpoint->binding();
            const auto operation = ++next_control;
            add_control(operation, PendingControl{promise, epoch, deadline, opts.stop,
                                                  [endpoint, binding, demand] {
                                                    if (endpoint->matches(binding))
                                                      endpoint->set_receive_demand(demand);
                                                  }});
            try {
              transport->set_receive_demand({operation, binding, demand, deadline, opts.stop});
            } catch (...) {
              erase_control(operation);
              throw;
            }
          },
          [promise](auto failure) { promise->set_exception(failure); }});
    return result;
  }

  void finish_removal(const std::shared_ptr<Endpoint>& endpoint, std::exception_ptr failure = {}) {
    const auto it = std::find_if(removals.begin(), removals.end(),
                                 [&](const auto& removal) { return removal.endpoint == endpoint; });
    if (it == removals.end()) {
      return;  // The room stop barrier already settled this incarnation.
    }
    auto waiters = std::move(it->waiters);
    removal_waiters -= waiters.size();
    removals.erase(it);
    for (auto& waiter : waiters) {
      if (failure) {
        waiter->set_exception(failure);
      } else {
        waiter->set_value();
      }
    }
  }

  void finish_all_removals() {
    while (!removals.empty()) {
      finish_removal(removals.back().endpoint);
    }
  }

  void close_entry(std::map<Key, Entry>::iterator it,
                   const std::shared_ptr<CompletionSource<void>>& promise) {
    if (removals.size() >= 1024 || removal_waiters >= 1024) {
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                     "RTC detachment queue exhausted");
    }
    auto entry = it->second;
    erase_track(it);  // Remove intent before detachment or a late publication ACK.
    entry.endpoint->finish(TrackState::closed);
    if (entry.completion) {
      entry.completion(error("RTC track operation cancelled", ErrorCode::cancelled));
    }
    refresh();
    detach_endpoint(entry.endpoint, promise);
  }

  void detach_endpoint(const std::shared_ptr<Endpoint>& endpoint,
                       std::shared_ptr<CompletionSource<void>> promise = {}) {
    if (removals.size() >= 1024 || (promise && removal_waiters >= 1024)) {
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                     "RTC detachment queue exhausted");
    }
    auto detached = transport->remove(endpoint->binding(), endpoint->publication);
    removals.push_back({endpoint, {}});
    if (promise) {
      removals.back().waiters.push_back(std::move(promise));
      ++removal_waiters;
    }
    auto weak = weak_from_this();
    std::move(detached).then([](std::function<void()> task) { task(); },
                             [weak, endpoint](AsyncResult<void> ready) {
                               std::exception_ptr failure;
                               try {
                                 ready.get();
                               } catch (...) {
                                 failure = std::current_exception();
                               }
                               if (auto state = weak.lock()) {
                                 std::lock_guard lock(state->mutex);
                                 if (!state->shutting_down) {
                                   state->completed_removals.emplace(endpoint, failure);
                                   state->wakeup->signal();
                                 }
                               }
                             });
  }

  void wait_removal(const std::shared_ptr<Endpoint>& endpoint, const TrackId& id, bool publication,
                    const std::shared_ptr<CompletionSource<void>>& promise) {
    // An endpoint handle targets its exact incarnation. An ID-only repeated close
    // targets the most recent detachment of that ID, never an older incarnation.
    auto it = std::find_if(removals.rbegin(), removals.rend(), [&](const auto& removal) {
      return endpoint ? removal.endpoint == endpoint
                      : removal.endpoint->descriptor.id == id &&
                            removal.endpoint->publication == publication;
    });
    if (it == removals.rend()) {
      promise->set_value();
      return;
    }
    if (removal_waiters >= 1024) {
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                     "RTC detachment waiters exhausted");
    }
    it->waiters.push_back(promise);
    ++removal_waiters;
  }

  AsyncResult<void> close_track(const std::shared_ptr<Endpoint>& endpoint) {
    auto promise = std::make_shared<CompletionSource<void>>();
    auto future = promise->get_result();
    post({[this, endpoint, promise] {
            const auto binding = endpoint->binding();
            auto it = tracks.find({binding.publication, binding.id});
            if (it != tracks.end() && it->second.endpoint == endpoint) {
              close_entry(it, promise);
            } else {
              wait_removal(endpoint, binding.id, binding.publication, promise);
            }
          },
          [promise](auto failure) { promise->set_exception(failure); }});
    return future;
  }

  AsyncResult<void> close_id(const TrackId& id, bool publication) {
    auto promise = std::make_shared<CompletionSource<void>>();
    auto future = promise->get_result();
    post({[this, id, publication, promise] {
            auto it = tracks.find({publication, id});
            if (it != tracks.end()) {
              close_entry(it, promise);
            } else {
              wait_removal({}, id, publication, promise);
            }
          },
          [promise](auto failure) { promise->set_exception(failure); }});
    return future;
  }

  AsyncResult<void> request_keyframe(const std::shared_ptr<Endpoint>& endpoint) {
    auto promise = std::make_shared<CompletionSource<void>>();
    auto future = promise->get_result();
    post({[this, endpoint, promise] {
            const auto binding = endpoint->binding();
            auto it = tracks.find({false, binding.id});
            if (it == tracks.end() || it->second.endpoint != endpoint ||
                endpoint->snapshot().state != TrackState::ready)
              throw SDKError(ErrorDomain::lifecycle, ErrorCode::stale_handle,
                             "RTC subscription is not ready");
            if (endpoint->descriptor.kind != MediaKind::video)
              throw std::invalid_argument("Audio has no keyframe request");
            endpoint->require_keyframe();
            send_pli(it->second);
            promise->set_value();
          },
          [promise](auto failure) { promise->set_exception(failure); }});
    return future;
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
        joining->set_value();
        joining.reset();
      }
      emit({RoomEventKind::connected, {}, {}, epoch, {}});
      return;
    }
    if (event.kind == TransportEventKind::control_completed) {
      auto it = controls.find(event.operation_id);
      if (it == controls.end() || it->second.epoch != event.binding.epoch)
        return;
      if (it->second.stop.stop_requested() || SteadyClock::now() >= it->second.deadline)
        it->second.promise->set_exception(error(
            "RTC control completion arrived after cancellation/deadline; remote outcome is unknown",
            ErrorCode::outcome_unknown));
      else if (event.control_error)
        it->second.promise->set_exception(
            std::make_exception_ptr(SDKError(ErrorDomain::signaling, *event.control_error,
                                             event.error, false, event.control_reason)));
      else {
        if (it->second.applied)
          it->second.applied();
        it->second.promise->set_value();
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
          emit({RoomEventKind::track_mute_changed, event.binding.id, {}, epoch, {}});
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
        emit({RoomEventKind::track_ready, event.binding.id, endpoint.descriptor, epoch, {}});
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
        emit(std::move(notification));
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
          auto promise = found->second.promise;
          erase_control(id);
          promise->set_exception(
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

  void run(std::stop_token stop) noexcept {
    while (!stop.stop_requested()) {
      const auto observed = wakeup->revision();
      ++controller_wakeups;
      std::deque<Command> pending;
      std::map<std::shared_ptr<Endpoint>, std::exception_ptr> failures, detached;
      {
        std::lock_guard lock(mutex);
        pending.swap(commands);
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
      for (auto& command : pending) {
        try {
          command.run();
        } catch (...) {
          command.cancel(std::current_exception());
        }
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
        wakeup->wait(observed, stop, next_deadline());
      } catch (const std::exception& e) {
        stop_all(ConnectionState::failed, e.what(), std::current_exception());
      } catch (...) {
        stop_all(ConnectionState::failed, "Unknown RTC transport failure");
      }
    }
    stop_all(ConnectionState::closed, "RTC room destroyed");
    std::deque<Command> pending;
    {
      std::lock_guard lock(mutex);
      pending.swap(commands);
    }
    for (auto& command : pending)
      command.cancel(error("RTC room destroyed"));
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

AsyncResult<void> PublishedTrack::set_muted(bool muted, ControlOptions options) {
  if (auto session = session_.lock())
    return session->set_muted(endpoint_->descriptor.id, muted, options, endpoint_);
  return detail::rejected<void>(detail::error("RTC room destroyed"));
}

void PublishedTrack::report_failure(std::exception_ptr failure) {
  if (auto session = session_.lock()) {
    session->report_failure(endpoint_, failure);
  }
}

AsyncResult<void> PublishedTrack::close() {
  if (auto session = session_.lock())
    return session->close_track(endpoint_);
  return detail::rejected<void>(detail::error("RTC room destroyed"));
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

AsyncResult<void> SubscribedTrack::request_keyframe() {
  if (auto session = session_.lock())
    return session->request_keyframe(endpoint_);
  return detail::rejected<void>(detail::error("RTC room destroyed"));
}

AsyncResult<void> SubscribedTrack::set_demand(ReceiveDemand demand, ControlOptions options) {
  if (auto session = session_.lock())
    return session->set_receive_demand(endpoint_->descriptor.id, demand, options, endpoint_);
  return detail::rejected<void>(detail::error("RTC room destroyed"));
}

void SubscribedTrack::report_failure(std::exception_ptr failure) {
  if (auto session = session_.lock()) {
    session->report_failure(endpoint_, failure);
  }
}

AsyncResult<void> SubscribedTrack::close() {
  if (auto session = session_.lock())
    return session->close_track(endpoint_);
  return detail::rejected<void>(detail::error("RTC room destroyed"));
}

RoomEngine::RoomEngine(TransportFactory factory)
    : state_(std::make_shared<detail::RoomEngineState>(std::move(factory))) {}

RoomEngine::~RoomEngine() {
  state_->set_handler({});
  state_->shutdown();
}

std::shared_ptr<RoomServices> RoomEngine::services() const {
  std::lock_guard lock(state_->mutex);
  if (!state_->services)
    throw SDKError(ErrorDomain::protocol, ErrorCode::unsupported,
                   "This transport does not provide room services");
  return state_->services;
}

AsyncResult<void> RoomEngine::join(RoomOptions options) {
  auto promise = std::make_shared<CompletionSource<void>>();
  auto future = promise->get_result();
  auto* s = state_.get();
  s->post({[s, options = std::move(options), promise] {
             if (s->connection != ConnectionState::disconnected &&
                 s->connection != ConnectionState::closed &&
                 s->connection != ConnectionState::failed) {
               throw std::logic_error("Leave the RTC room before joining again");
             }
             if (!options.max_queued_bytes || options.max_queued_bytes > 1024ull * 1024 * 1024 ||
                 !options.max_tracks || options.max_tracks > 1024 || !options.max_events ||
                 options.max_events > 65536 ||
                 options.operation_timeout <= std::chrono::milliseconds::zero() ||
                 options.operation_timeout > std::chrono::minutes(5) ||
                 (options.stats_interval.count() != 0 &&
                  (options.stats_interval < std::chrono::milliseconds(200) ||
                   options.stats_interval > std::chrono::minutes(1)))) {
               throw std::invalid_argument("Invalid RTC room options");
             }
             if (options.subscription_permissions)
               detail::validate_subscription_permissions(*options.subscription_permissions);
             if (s->connection != ConnectionState::disconnected) {
               auto replacement = s->transport_factory();
               if (!replacement) {
                 throw std::invalid_argument("RTC transport factory returned null");
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
             try {
               s->transport->attach_wakeup(s->wakeup);
               s->transport->set_queue_budget(s->budget);
               s->transport->start(options, s->epoch);
             } catch (...) {
               s->stop_all(ConnectionState::failed, "RTC transport start failed",
                           std::current_exception());
               throw;
             }
             s->joining = promise;
           },
           [promise](auto failure) { promise->set_exception(failure); }});
  return future;
}

AsyncResult<std::shared_ptr<PublishedTrack>> RoomEngine::publish(TrackDescriptor descriptor) {
  auto promise = std::make_shared<CompletionSource<std::shared_ptr<PublishedTrack>>>();
  auto future = promise->get_result();
  auto* s = state_.get();
  s->post(
      {[s, weak = std::weak_ptr(state_), descriptor = std::move(descriptor), promise]() mutable {
         auto endpoint = s->make_endpoint(std::move(descriptor), true);
         auto track = std::shared_ptr<PublishedTrack>(new PublishedTrack(endpoint, weak));
         s->add(endpoint, [s, promise, track](auto failure) {
           if (failure)
             promise->set_exception(failure);
           else
             promise->set_value(track);
         });
       },
       [s, promise](auto failure) { promise->set_exception(failure); }});
  return future;
}

AsyncResult<std::shared_ptr<SubscribedTrack>> RoomEngine::subscribe(TrackDescriptor descriptor) {
  auto promise = std::make_shared<CompletionSource<std::shared_ptr<SubscribedTrack>>>();
  auto future = promise->get_result();
  auto* s = state_.get();
  s->post(
      {[s, weak = std::weak_ptr(state_), descriptor = std::move(descriptor), promise]() mutable {
         auto endpoint = s->make_endpoint(std::move(descriptor), false);
         auto track = std::shared_ptr<SubscribedTrack>(new SubscribedTrack(endpoint, weak));
         s->add(endpoint, [s, promise, track](auto failure) {
           if (failure)
             promise->set_exception(failure);
           else
             promise->set_value(track);
         });
       },
       [s, promise](auto failure) { promise->set_exception(failure); }});
  return future;
}

AsyncResult<void> RoomEngine::leave() {
  auto promise = std::make_shared<CompletionSource<void>>();
  auto future = promise->get_result();
  auto* s = state_.get();
  s->post({[s, promise] {
             if (s->connection != ConnectionState::closed)
               s->stop_all(ConnectionState::closed, "RTC room left");
             promise->set_value();
           },
           [promise](auto failure) { promise->set_exception(failure); }});
  return future;
}

AsyncResult<void> RoomEngine::unpublish(const TrackId& id) {
  return state_->close_id(id, true);
}

AsyncResult<void> RoomEngine::unsubscribe(const TrackId& id) {
  return state_->close_id(id, false);
}

AsyncResult<void> RoomEngine::set_muted(const TrackId& id, bool muted, ControlOptions options) {
  return state_->set_muted(id, muted, options);
}

AsyncResult<void> RoomEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                                 ControlOptions options) {
  return state_->set_receive_demand(id, demand, options);
}

AsyncResult<void> RoomEngine::update_local_participant(ParticipantUpdate update,
                                                       ControlOptions options) {
  auto promise = std::make_shared<CompletionSource<void>>();
  auto future = promise->get_result();
  try {
    size_t bytes = update.name.size() + update.metadata.size();
    if (update.name.size() > 1024 || update.metadata.size() > 65536 ||
        update.attributes.size() > 128 || !livekit::sdk::detail::valid_utf8(update.name) ||
        !livekit::sdk::detail::valid_utf8(update.metadata) || options.timeout.count() < 0 ||
        options.timeout > std::chrono::minutes(5))
      throw std::invalid_argument("Invalid participant update");
    for (const auto& [key, value] : update.attributes) {
      if (key.empty() || key.size() > 256 || value.size() > 16384 ||
          !livekit::sdk::detail::valid_utf8(key) || !livekit::sdk::detail::valid_utf8(value))
        throw std::invalid_argument("Invalid participant attribute");
      bytes += key.size() + value.size();
    }
    if (bytes > 65536)
      throw std::invalid_argument("Participant update exceeds 64 KiB");
    auto* s = state_.get();
    const auto submitted = SteadyClock::now();
    s->post(
        {[s, promise, update = std::move(update), options, submitted]() mutable {
           if (s->connection != ConnectionState::connected)
             std::rethrow_exception(detail::error("Room is not connected"));
           if (!s->transport->capabilities().participant_update_ack) {
             std::rethrow_exception(
                 detail::error("Backend does not support acknowledged participant updates",
                               ErrorCode::unsupported));
           }
           if (options.stop.stop_requested())
             std::rethrow_exception(detail::error("Participant update cancelled before dispatch",
                                                  ErrorCode::cancelled));
           const auto deadline =
               submitted +
               (options.timeout.count() ? options.timeout : s->options.operation_timeout);
           if (SteadyClock::now() >= deadline)
             std::rethrow_exception(
                 detail::error("Participant update expired before dispatch", ErrorCode::timeout));
           if (s->controls.size() >= 128 || s->next_control == std::numeric_limits<uint32_t>::max())
             std::rethrow_exception(detail::error("Participant update capacity exhausted",
                                                  ErrorCode::resource_exhausted));
           const auto id = ++s->next_control;
           s->add_control(id, detail::RoomEngineState::PendingControl{promise, s->epoch, deadline,
                                                                      options.stop});
           try {
             s->transport->update_participant(
                 {id, s->epoch, std::move(update), deadline, options.stop});
           } catch (...) {
             s->erase_control(id);
             throw;
           }
         },
         [promise](auto failure) { promise->set_exception(failure); }});
  } catch (...) {
    promise->set_exception(std::current_exception());
  }
  return future;
}

AsyncResult<void> RoomEngine::set_subscription_permissions(SubscriptionPermissions policy,
                                                           ControlOptions options) {
  auto promise = std::make_shared<CompletionSource<void>>();
  auto future = promise->get_result();
  try {
    detail::validate_subscription_permissions(policy);
    if (options.timeout.count() < 0 || options.timeout > std::chrono::minutes(5))
      throw std::invalid_argument("Invalid subscription permission timeout");
    auto* s = state_.get();
    const auto submitted = SteadyClock::now();
    s->post(
        {[s, promise, policy = std::move(policy), options, submitted]() mutable {
           if (s->connection != ConnectionState::connected)
             std::rethrow_exception(detail::error("Room is not connected"));
           if (options.stop.stop_requested())
             std::rethrow_exception(detail::error("Permission update cancelled before dispatch",
                                                  ErrorCode::cancelled));
           const auto deadline =
               submitted +
               (options.timeout.count() ? options.timeout : s->options.operation_timeout);
           if (SteadyClock::now() >= deadline)
             std::rethrow_exception(
                 detail::error("Permission update expired before dispatch", ErrorCode::timeout));
           if (s->controls.size() >= 128 || s->next_control == std::numeric_limits<uint32_t>::max())
             std::rethrow_exception(
                 detail::error("RTC control capacity exhausted", ErrorCode::resource_exhausted));
           const auto id = ++s->next_control;
           s->add_control(id, detail::RoomEngineState::PendingControl{promise, s->epoch, deadline,
                                                                      options.stop});
           try {
             s->transport->set_subscription_permissions(
                 {id, s->epoch, std::move(policy), deadline, options.stop});
           } catch (...) {
             s->erase_control(id);
             throw;
           }
         },
         [promise](auto failure) { promise->set_exception(failure); }});
  } catch (...) {
    promise->set_exception(std::current_exception());
  }
  return future;
}

RoomSnapshot RoomEngine::snapshot() const {
  std::lock_guard lock(state_->mutex);
  return state_->status;
}

void RoomEngine::set_event_handler(std::function<void(const RoomEvent&)> handler) {
  state_->set_handler(std::move(handler));
}

void RoomEngine::post_notification(std::function<void()> callback) {
  state_->post_notification(std::move(callback));
}

}  // namespace livekit::sdk::rtc
