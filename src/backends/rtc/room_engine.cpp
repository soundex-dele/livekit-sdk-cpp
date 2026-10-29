#include "livekit/rtc/room_engine.hpp"
#include "backends/rtc/endpoint.hpp"
#include "backends/rtc/bounded_events.hpp"
#include "backends/rtc/subscription_policy.hpp"
#include "livekit/error.hpp"
#include "support/utf8.hpp"
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
std::future<T> rejected(std::exception_ptr failure) {
  std::promise<T> result;
  result.set_exception(failure);
  return result.get_future();
}
}  // namespace

struct RoomEngineState {
  explicit RoomEngineState(RoomEngine::TransportFactory factory)
      : transport_factory(std::move(factory)) {
    if (!transport_factory) {
      throw std::invalid_argument("RTC room requires a transport factory");
    }
    transport = transport_factory();
    if (!transport)
      throw std::invalid_argument("RTC room requires a transport");
    services = transport->services();
    notifications = vi::TaskQueue::create("rtc.room.notifications");
    worker = std::jthread([this](std::stop_token stop) { run(stop); });
  }

  ~RoomEngineState() {
    set_handler({});
    shutdown();
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
    std::future<void> detached;
    std::vector<std::shared_ptr<std::promise<void>>> waiters;
  };

  using Key = std::pair<bool, TrackId>;
  RoomEngine::TransportFactory transport_factory;
  std::unique_ptr<RoomTransport> transport;
  std::shared_ptr<RoomServices> services;
  std::jthread worker;
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  std::deque<Command> commands;
  std::deque<RoomEvent> events;
  std::unique_ptr<vi::TaskQueue> notifications;
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

  std::mutex completion_mutex;
  std::function<void()> completion_wakeup;

  void notify_completion() {
    std::lock_guard lock(completion_mutex);
    if (completion_wakeup) {
      completion_wakeup();
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
  std::vector<Removal> removals;  // Controller-owned native detachment barriers.
  size_t removal_waiters = 0;
  std::shared_ptr<std::promise<void>> joining;
  SteadyClock::time_point join_deadline{};

  struct PendingControl {
    std::shared_ptr<std::promise<void>> promise;
    Epoch epoch;
    SteadyClock::time_point deadline;
    std::stop_token stop;
    std::function<void()> applied;
  };

  uint32_t next_control = 0;
  std::map<uint32_t, PendingControl> controls;

  void interrupt_controls() {
    for (const auto& [id, pending] : controls)
      pending.promise->set_exception(
          error("RTC control interrupted; remote outcome is unknown", ErrorCode::outcome_unknown));
    controls.clear();
  }

  void shutdown() {
    {
      std::lock_guard lock(mutex);
      shutting_down = true;
    }
    worker.request_stop();
    changed.notify_all();
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
        changed.notify_all();
        return true;
      }
    }
    command.cancel(failure);
    return false;
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
    status.publications = status.subscriptions = 0;
    for (const auto& [key, entry] : tracks) {
      if (key.first)
        ++status.publications;
      else
        ++status.subscriptions;
    }
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
    const auto failure = cause ? cause : error(message);
    auto joining_result = std::move(joining);
    for (auto& [key, entry] : tracks) {
      entry.endpoint->finish(
          state == ConnectionState::failed ? TrackState::failed : TrackState::closed, message);
      if (entry.completion) {
        entry.completion(failure);
        entry.completion = {};
      }
    }
    tracks.clear();
    transport->stop();
    // stop() is the final native barrier, including removals queued at shutdown.
    complete_removals(true);
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
    auto entry = std::move(it->second);
    tracks.erase(it);
    entry.endpoint->finish(TrackState::failed, message);
    if (entry.completion)
      entry.completion(failure);
    // Native track cleanup is best-effort here. A session-wide failure must be
    // reported by the adapter as interrupted/terminal_error, not inferred from
    // an individual send or codec negotiation failure.
    try {
      transport->remove(entry.endpoint->binding(), key.first);
    } catch (...) {
    }
    emit({RoomEventKind::track_failed, key.second, {}, epoch, message});
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
    try {
      if (connection == ConnectionState::connected)
        dispatch(it->second);
    } catch (...) {
      tracks.erase(it);
      throw;
    }
    refresh();
  }

  std::shared_ptr<Endpoint> make_endpoint(TrackDescriptor descriptor, bool publication) {
    if (!budget)
      throw std::logic_error("Join the RTC room before adding tracks");
    validate_descriptor(descriptor, budget->limit, publication);
    BindingToken binding{descriptor.id, epoch, ++next_incarnation, publication};
    return std::make_shared<Endpoint>(std::move(descriptor), std::move(binding), budget);
  }

  std::future<void> set_muted(TrackId id, bool muted, ControlOptions opts,
                              std::shared_ptr<Endpoint> expected = {}) {
    auto promise = std::make_shared<std::promise<void>>();
    auto result = promise->get_future();
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
              controls.emplace(operation, PendingControl{promise, epoch, deadline, opts.stop});
              transport->set_muted({operation, endpoint->binding(), muted, deadline, opts.stop});
            } catch (...) {
              controls.erase(operation);
              endpoint->set_muted(previous);
              throw;
            }
          },
          [promise](auto failure) { promise->set_exception(failure); }});
    return result;
  }

  std::future<void> set_receive_demand(TrackId id, ReceiveDemand demand, ControlOptions opts,
                                       std::shared_ptr<Endpoint> expected = {}) {
    auto promise = std::make_shared<std::promise<void>>();
    auto result = promise->get_future();
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
            controls.emplace(operation, PendingControl{promise, epoch, deadline, opts.stop,
                                                       [endpoint, binding, demand] {
                                                         if (endpoint->matches(binding))
                                                           endpoint->set_receive_demand(demand);
                                                       }});
            try {
              transport->set_receive_demand({operation, binding, demand, deadline, opts.stop});
            } catch (...) {
              controls.erase(operation);
              throw;
            }
          },
          [promise](auto failure) { promise->set_exception(failure); }});
    return result;
  }

  void complete_removals(bool stopped = false) {
    for (auto it = removals.begin(); it != removals.end();) {
      if (!stopped && it->detached.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        ++it;
        continue;
      }
      std::exception_ptr failure;
      if (!stopped) {
        try {
          it->detached.get();
        } catch (...) {
          failure = std::current_exception();
        }
      }
      for (auto& waiter : it->waiters) {
        if (failure) {
          waiter->set_exception(failure);
        } else {
          waiter->set_value();
        }
      }
      removal_waiters -= it->waiters.size();
      it = removals.erase(it);
    }
  }

  void close_entry(std::map<Key, Entry>::iterator it,
                   const std::shared_ptr<std::promise<void>>& promise) {
    if (removals.size() >= 1024 || removal_waiters >= 1024) {
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                     "RTC detachment queue exhausted");
    }
    auto entry = std::move(it->second);
    tracks.erase(it);  // Remove intent before detachment or a late publication ACK.
    entry.endpoint->finish(TrackState::closed);
    if (entry.completion) {
      entry.completion(error("RTC track operation cancelled", ErrorCode::cancelled));
    }
    refresh();
    auto detached = transport->remove(entry.endpoint->binding(), entry.endpoint->publication);
    removals.push_back({std::move(entry.endpoint), std::move(detached), {promise}});
    ++removal_waiters;
    complete_removals();
  }

  void wait_removal(const std::shared_ptr<Endpoint>& endpoint, const TrackId& id, bool publication,
                    const std::shared_ptr<std::promise<void>>& promise) {
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

  std::future<void> close_track(const std::shared_ptr<Endpoint>& endpoint) {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
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

  std::future<void> close_id(const TrackId& id, bool publication) {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
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

  std::future<void> request_keyframe(const std::shared_ptr<Endpoint>& endpoint) {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
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
      controls.erase(it);
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
        endpoint.finish(
            event.kind == TransportEventKind::removed ? TrackState::closed : TrackState::failed,
            event.error);
        if (entry.completion)
          entry.completion(error(event.error, ErrorCode::request_failed));
        emit({event.kind == TransportEventKind::removed ? RoomEventKind::track_removed
                                                        : RoomEventKind::track_failed,
              event.binding.id, endpoint.descriptor, epoch, event.error});
        tracks.erase(it);
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
      default:
        break;
    }
  }

  void tick() {
    complete_removals();
    const auto now = SteadyClock::now();
    for (auto it = controls.begin(); it != controls.end();) {
      if (it->second.stop.stop_requested() || now >= it->second.deadline) {
        it->second.promise->set_exception(
            error("RTC control cancelled or timed out after dispatch; remote outcome is unknown",
                  ErrorCode::outcome_unknown));
        it = controls.erase(it);
      } else
        ++it;
    }
    if (joining && now >= join_deadline) {
      stop_all(ConnectionState::failed, "RTC join timed out",
               error("RTC join timed out", ErrorCode::timeout));
      return;
    }
    for (auto it = tracks.begin(); it != tracks.end();) {
      const auto key = it->first;
      auto& entry = it->second;
      if (entry.completion && now >= entry.deadline) {
        ++it;
        fail_track(key, error("RTC track operation timed out", ErrorCode::timeout),
                   "RTC track operation timed out");
        continue;
      }
      ++it;
      if (connection != ConnectionState::connected ||
          entry.endpoint->snapshot().state != TrackState::ready)
        continue;
      try {
        if (entry.endpoint->publication) {
          // Bounded work per track, then yield to the next. Native pacing and
          // congestion control remain authoritative; this is admission fairness.
          for (uint32_t n = 0; n < entry.endpoint->descriptor.priority; ++n) {
            auto item = entry.endpoint->front();
            if (!item)
              break;
            const auto sent = transport->send(entry.endpoint->binding(), item->frame);
            if (sent == SendResult::would_block)
              break;
            entry.endpoint->consumed(item->serial, sent == SendResult::dropped);
          }
        } else if (entry.endpoint->descriptor.kind == MediaKind::video) {
          const auto feedback = entry.endpoint->feedback();
          if (std::any_of(feedback.encodings.begin(), feedback.encodings.end(),
                          [](const auto& item) {
                            return item.second.keyframe_required && item.second.active;
                          }))
            send_pli(entry);
        }
      } catch (const std::exception& e) {
        fail_track(key, std::current_exception(), e.what());
      }
    }
    refresh();
  }

  void run(std::stop_token stop) noexcept {
    while (!stop.stop_requested()) {
      std::deque<Command> pending;
      {
        std::lock_guard lock(mutex);
        pending.swap(commands);
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
          for (auto& event : transport->poll(std::chrono::milliseconds(2), stop)) {
            if (connection == ConnectionState::closed || connection == ConnectionState::failed)
              break;
            handle(std::move(event));
          }
          tick();
        } else {
          std::unique_lock lock(mutex);
          changed.wait_for(lock, stop, std::chrono::milliseconds(10),
                           [&] { return !commands.empty(); });
        }
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

std::future<void> PublishedTrack::set_muted(bool muted, ControlOptions options) {
  if (auto session = session_.lock())
    return session->set_muted(endpoint_->descriptor.id, muted, options, endpoint_);
  return detail::rejected<void>(detail::error("RTC room destroyed"));
}

std::future<void> PublishedTrack::close() {
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

std::future<void> SubscribedTrack::request_keyframe() {
  if (auto session = session_.lock())
    return session->request_keyframe(endpoint_);
  return detail::rejected<void>(detail::error("RTC room destroyed"));
}

std::future<void> SubscribedTrack::set_demand(ReceiveDemand demand, ControlOptions options) {
  if (auto session = session_.lock())
    return session->set_receive_demand(endpoint_->descriptor.id, demand, options, endpoint_);
  return detail::rejected<void>(detail::error("RTC room destroyed"));
}

std::future<void> SubscribedTrack::close() {
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

void RoomEngine::set_completion_wakeup(std::function<void()> wakeup) {
  std::lock_guard lock(state_->completion_mutex);
  state_->completion_wakeup = std::move(wakeup);
}

std::future<void> RoomEngine::join(RoomOptions options) {
  auto promise = std::make_shared<std::promise<void>>();
  auto future = promise->get_future();
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

std::future<std::shared_ptr<PublishedTrack>> RoomEngine::publish(TrackDescriptor descriptor) {
  auto promise = std::make_shared<std::promise<std::shared_ptr<PublishedTrack>>>();
  auto future = promise->get_future();
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
           s->notify_completion();
         });
       },
       [s, promise](auto failure) {
         promise->set_exception(failure);
         s->notify_completion();
       }});
  return future;
}

std::future<std::shared_ptr<SubscribedTrack>> RoomEngine::subscribe(TrackDescriptor descriptor) {
  auto promise = std::make_shared<std::promise<std::shared_ptr<SubscribedTrack>>>();
  auto future = promise->get_future();
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
           s->notify_completion();
         });
       },
       [s, promise](auto failure) {
         promise->set_exception(failure);
         s->notify_completion();
       }});
  return future;
}

std::future<void> RoomEngine::leave() {
  auto promise = std::make_shared<std::promise<void>>();
  auto future = promise->get_future();
  auto* s = state_.get();
  s->post({[s, promise] {
             if (s->connection != ConnectionState::closed)
               s->stop_all(ConnectionState::closed, "RTC room left");
             promise->set_value();
           },
           [promise](auto failure) { promise->set_exception(failure); }});
  return future;
}

std::future<void> RoomEngine::unpublish(const TrackId& id) {
  return state_->close_id(id, true);
}

std::future<void> RoomEngine::unsubscribe(const TrackId& id) {
  return state_->close_id(id, false);
}

std::future<void> RoomEngine::set_muted(const TrackId& id, bool muted, ControlOptions options) {
  return state_->set_muted(id, muted, options);
}

std::future<void> RoomEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                                  ControlOptions options) {
  return state_->set_receive_demand(id, demand, options);
}

std::future<void> RoomEngine::update_local_participant(ParticipantUpdate update,
                                                        ControlOptions options) {
  auto promise = std::make_shared<std::promise<void>>();
  auto future = promise->get_future();
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
           s->controls.emplace(
               id, detail::RoomEngineState::PendingControl{promise, s->epoch, deadline, options.stop});
           try {
             s->transport->update_participant(
                 {id, s->epoch, std::move(update), deadline, options.stop});
           } catch (...) {
             s->controls.erase(id);
             throw;
           }
         },
         [promise](auto failure) { promise->set_exception(failure); }});
  } catch (...) {
    promise->set_exception(std::current_exception());
  }
  return future;
}

std::future<void> RoomEngine::set_subscription_permissions(SubscriptionPermissions policy,
                                                            ControlOptions options) {
  auto promise = std::make_shared<std::promise<void>>();
  auto future = promise->get_future();
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
           s->controls.emplace(
               id, detail::RoomEngineState::PendingControl{promise, s->epoch, deadline, options.stop});
           try {
             s->transport->set_subscription_permissions(
                 {id, s->epoch, std::move(policy), deadline, options.stop});
           } catch (...) {
             s->controls.erase(id);
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
