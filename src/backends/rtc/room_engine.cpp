#include "backends/rtc/room_engine.hpp"
#include "backends/rtc/endpoint.hpp"
#include "backends/rtc/subscription_policy.hpp"
#include "support/utf8.hpp"
#include "support/ready_set.hpp"
#include "support/deadline_index.hpp"
#include <algorithm>
#include <limits>

namespace livekit::sdk::rtc {
namespace detail {
namespace {
std::exception_ptr error(std::string message, ErrorCode code = ErrorCode::session_closed) {
  return std::make_exception_ptr(SDKError(ErrorDomain::lifecycle, code, std::move(message)));
}
}  // namespace

// Result delivery is a separate phase from protocol mutation. A transport
// batch may contain connected followed by the initial roster; observers and
// synchronous adapters must see the whole batch before consuming join success.
struct PendingCompletion : std::enable_shared_from_this<PendingCompletion> {
  virtual ~PendingCompletion() = default;
  virtual void complete() noexcept = 0;
  std::shared_ptr<PendingCompletion> next;
};

struct CompletionBatch {
  std::shared_ptr<PendingCompletion> head;
  PendingCompletion* tail = nullptr;

  void enqueue(std::shared_ptr<PendingCompletion> pending) noexcept {
    auto* last = pending.get();
    if (tail) {
      tail->next = std::move(pending);
    } else {
      head = std::move(pending);
    }
    tail = last;
  }

  void flush() noexcept {
    while (head) {
      auto pending = std::move(head);
      head = std::move(pending->next);
      if (!head) {
        tail = nullptr;
      }
      pending->complete();
    }
  }
};

// Controller-owned protocol state survives initiation until ACK, cancellation,
// timeout or native detachment. Failures cross threads in the completion itself.
template <class T>
struct PendingOperation : PendingCompletion {
  PendingOperation(CompletionBatch& batch, RoomEngine::Completed<T> callback)
      : completion(std::move(callback)), batch(batch) {}

  using Return = Result<T>;
  RoomEngine::Completed<T> completion;
  CompletionBatch& batch;
  std::optional<Return> value;
  std::exception_ptr failure;
  bool settled = false;  // Owner serializes protocol access.

  template <class... Args>
  void succeed(Args&&... args) {
    if (!settled) {
      if constexpr (std::is_void_v<T>) {
        value.emplace(Result<void>::success());
      } else {
        value.emplace(Result<T>::success(std::forward<Args>(args)...));
      }
      settled = true;
      batch.enqueue(shared_from_this());
    }
  }

  void fail(std::exception_ptr failure) {
    if (!settled) {
      if constexpr (std::is_void_v<T>) {
        this->failure = std::move(failure);
      } else {
        value.emplace(Result<T>::failure(describe_failure(failure)));
      }
      settled = true;
      batch.enqueue(shared_from_this());
    }
  }

  void complete() noexcept override {
    auto callback = std::move(completion);
    if (callback) {
      callback(failure ? Return::failure(describe_failure(failure)) : std::move(*value));
    }
  }
};

struct RoomEngineState {
  RoomEngineState(RoomEngine::TransportFactory factory, RoomEngine::Hooks hooks)
      : transport_factory(std::move(factory)),
        hooks(std::move(hooks)),
        wakeup(this->hooks.wakeup),
        ready_work(std::make_shared<livekit::sdk::detail::ReadySet<WorkKey>>(wakeup, 1024 + 128)) {
    try {
      if (transport_factory) {
        transport = transport_factory();
      }
      if (transport) {
        transport->attach_wakeup(wakeup);
        services = transport->services();
        this->hooks.attach_services(*transport);
      }
    } catch (...) {
      initialization_failure = std::current_exception();
      services.reset();
      transport.reset();
    }
  }

  ~RoomEngineState() {
    stop_all(ConnectionState::closed, "RTC room destroyed");
    pending_completions.flush();
  }

  struct Entry {
    std::shared_ptr<Endpoint> endpoint;
    SteadyClock::time_point deadline;
    std::function<void(std::exception_ptr)> completion;
    SteadyClock::time_point last_pli{};
  };

  struct Removal {
    std::shared_ptr<Endpoint> endpoint;
    std::vector<std::shared_ptr<PendingOperation<void>>> waiting;
  };

  using Key = std::pair<bool, TrackId>;
  enum class WorkKind { track, control };
  using WorkKey = std::pair<WorkKind, uint64_t>;
  RoomEngine::TransportFactory transport_factory;
  std::unique_ptr<RoomTransport> transport;
  std::exception_ptr initialization_failure;
  std::shared_ptr<RoomServices> services;
  RoomEngine::Hooks hooks;
  std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup;
  std::shared_ptr<livekit::sdk::detail::ReadySet<WorkKey>> ready_work;
  CompletionBatch pending_completions;
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
  std::shared_ptr<PendingOperation<void>> joining;
  SteadyClock::time_point join_deadline{};

  struct PendingControl {
    std::shared_ptr<PendingOperation<void>> pending;
    Epoch epoch;
    SteadyClock::time_point deadline;
    std::stop_token stop;
    std::function<void()> applied;
    std::unique_ptr<std::stop_callback<std::function<void()>>> cancellation;
  };

  uint32_t next_control = 0;
  std::map<uint32_t, PendingControl> controls;

  bool add_control(uint32_t id, PendingControl control) {
    auto pending = control.pending;
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

  template <class Function, class T>
  void run_request(Function&& request, const std::shared_ptr<PendingOperation<T>>& operation) {
    try {
      request();
    } catch (...) {
      operation->fail(std::current_exception());
    }
  }

  template <class T>
  void deferred(const std::shared_ptr<PendingOperation<T>>&) {
    pending_completions.flush();
    wakeup->signal();
  }

  void report_failure(std::shared_ptr<Endpoint> endpoint, std::exception_ptr failure) {
    const Key key{endpoint->publication, endpoint->descriptor.id};
    const auto found = tracks.find(key);
    if (found != tracks.end() && found->second.endpoint == endpoint) {
      fail_track(key, failure, describe_failure(failure)->message);
    }
    pending_completions.flush();
  }

  void refresh(std::string message = {}) {
    status.state = connection;
    status.epoch = epoch;
    status.capabilities = transport ? transport->capabilities() : BackendCapabilities{};
    if (connection == ConnectionState::closed || connection == ConnectionState::failed ||
        (status.session && status.session->epoch != epoch)) {
      status.session.reset();
    }
    status.queued_bytes = budget ? budget->used.load() : 0;
    status.publications = publication_count;
    status.subscriptions = subscription_count;
    status.controller_wakeups = controller_wakeups;
    status.tracks_serviced = tracks_serviced;
    status.scheduled_deadlines = deadlines.size() + bool(joining);
    status.ready_work = ready_work->pending();
    if (!message.empty()) {
      status.error = std::move(message);
    }
  }

  bool emit_track(RoomEvent event, bool publication, bool fatal_overflow = true) {
    event.publication = publication;
    return emit(std::move(event), fatal_overflow);
  }

  bool emit(RoomEvent event, bool fatal_overflow = true) {
    return hooks.event(std::move(event), false, fatal_overflow);
  }

  void stop_all(ConnectionState state, const std::string& message,
                std::exception_ptr cause = {}) noexcept {
    connection = state;
    // Retain pending results until native stop and the terminal snapshot are
    // visible. Deferred consumers may run as soon as a promise is settled.
    auto stopped_controls = std::move(controls);
    controls.clear();
    const auto failure =
        normalized_failure(cause ? cause : error(message), {"room", {}, {}, epoch});
    const auto info = describe_failure(failure);
    auto joining_result = std::exchange(joining, nullptr);
    auto stopped_tracks = std::move(tracks);
    tracks.clear();
    for (auto& [key, entry] : stopped_tracks) {
      entry.endpoint->finish(
          state == ConnectionState::failed ? TrackState::failed : TrackState::closed, message,
          state == ConnectionState::failed ? info : nullptr);
    }
    track_keys.clear();
    publication_count = subscription_count = 0;
    deadlines.clear();
    ready_work->clear();
    if (transport) {
      transport->stop();
    }
    refresh(state == ConnectionState::failed ? message : "");
    RoomEvent terminal{
        state == ConnectionState::failed ? RoomEventKind::failed : RoomEventKind::closed,
        {},
        {},
        epoch,
        message};
    if (state == ConnectionState::failed) {
      terminal.failure = info;
    }
    hooks.event(std::move(terminal), true, false);
    // Completion observes the terminal snapshot and transport shutdown barrier.
    // stop() also covers native removals already queued before shutdown.
    finish_all_removals();
    for (auto& [key, entry] : stopped_tracks) {
      if (entry.completion) {
        entry.completion(failure);
      }
    }
    for (auto& [id, pending] : stopped_controls) {
      pending.pending->fail(
          error("RTC control interrupted; remote outcome is unknown", ErrorCode::outcome_unknown));
    }
    if (joining_result) {
      joining_result->fail(failure);
    }
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
    if (it == tracks.end()) {
      return;
    }
    auto entry = it->second;
    erase_track(it);
    failure = normalized_failure(failure, {"track", key.second, {}, epoch});
    const auto info = describe_failure(failure);
    entry.endpoint->finish(TrackState::failed, info->message, info);
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
    if (entry.completion) {
      entry.completion(failure);
    }
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
      if (connection == ConnectionState::connected) {
        dispatch(it->second);
      }
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

  void set_muted(TrackId id, bool muted, ControlOptions opts, RoomEngine::Completed<void> completed,
                 std::shared_ptr<Endpoint> expected = {}) {
    auto operation =
        std::make_shared<PendingOperation<void>>(pending_completions, std::move(completed));
    auto pending = operation;
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
    return deferred(operation);
  }

  void set_receive_demand(TrackId id, ReceiveDemand demand, ControlOptions opts,
                          RoomEngine::Completed<void> completed,
                          std::shared_ptr<Endpoint> expected = {}) {
    auto operation =
        std::make_shared<PendingOperation<void>>(pending_completions, std::move(completed));
    auto pending = operation;
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
                                                       if (endpoint->matches(binding)) {
                                                         endpoint->set_receive_demand(demand);
                                                       }
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
    return deferred(operation);
  }

  void finish_removal(const std::shared_ptr<Endpoint>& endpoint, std::exception_ptr failure = {}) {
    const auto it = std::find_if(removals.begin(), removals.end(),
                                 [&](const auto& removal) { return removal.endpoint == endpoint; });
    if (it == removals.end()) {
      return;  // The room stop barrier already settled this incarnation.
    }
    auto waiters = std::move(it->waiting);
    removals.erase(it);
    for (const auto& waiting : waiters) {
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

  void close_entry(std::map<Key, Entry>::iterator it,
                   std::shared_ptr<PendingOperation<void>> pending) {
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
                       std::shared_ptr<PendingOperation<void>> pending = {}) {
    if (removals.size() >= 1024) {
      if (pending) {
        pending->fail(error("RTC detachment queue exhausted", ErrorCode::resource_exhausted));
      }
      return;
    }
    removals.push_back({endpoint, {}});
    if (pending) {
      removals.back().waiting.push_back(std::move(pending));
    }
    try {
      transport->remove(endpoint->binding(), endpoint->publication, hooks.removal(endpoint));
    } catch (...) {
      finish_removal(endpoint, std::current_exception());
    }
  }

  void wait_removal(const std::shared_ptr<Endpoint>& endpoint, const TrackId& id, bool publication,
                    std::shared_ptr<PendingOperation<void>> pending) {
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
    if (it->waiting.size() >= 129) {
      pending->fail(error("RTC detachment waiter limit exceeded", ErrorCode::resource_exhausted));
      return;
    }
    it->waiting.push_back(std::move(pending));
  }

  void close_track(const std::shared_ptr<Endpoint>& endpoint,
                   RoomEngine::Completed<void> completed) {
    auto operation =
        std::make_shared<PendingOperation<void>>(pending_completions, std::move(completed));
    auto pending = operation;
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
    return deferred(operation);
  }

  void close_id(const TrackId& id, bool publication, RoomEngine::Completed<void> completed) {
    auto operation =
        std::make_shared<PendingOperation<void>>(pending_completions, std::move(completed));
    auto pending = operation;
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
    return deferred(operation);
  }

  void request_keyframe(const std::shared_ptr<Endpoint>& endpoint,
                        RoomEngine::Completed<void> completed) {
    auto operation =
        std::make_shared<PendingOperation<void>>(pending_completions, std::move(completed));
    auto pending = operation;
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
    return deferred(operation);
  }

  void send_pli(Entry& entry) {
    const auto now = SteadyClock::now();
    if (now - entry.last_pli < std::chrono::milliseconds(250)) {
      return;
    }
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
          connection == ConnectionState::reconnecting) {
        return;
      }
      connection = ConnectionState::reconnecting;
      interrupt_controls();
      ++epoch;
      for (auto& [key, entry] : tracks) {
        entry.endpoint->rebind(epoch);
      }
      refresh();
      emit({RoomEventKind::reconnecting, {}, {}, epoch, event.error});
      return;
    }
    if (event.kind == TransportEventKind::connected) {
      if (connection != ConnectionState::connecting &&
          connection != ConnectionState::reconnecting) {
        return;
      }
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
      emit({RoomEventKind::connected, {}, {}, epoch, {}});
      if (auto pending = std::exchange(joining, {})) {
        pending->succeed();
      }
      return;
    }
    if (event.kind == TransportEventKind::control_completed) {
      auto it = controls.find(event.operation_id);
      if (it == controls.end() || it->second.epoch != event.binding.epoch) {
        return;
      }
      if (it->second.stop.stop_requested() || SteadyClock::now() >= it->second.deadline) {
        it->second.pending->fail(error(
            "RTC control completion arrived after cancellation/deadline; remote outcome is unknown",
            ErrorCode::outcome_unknown));
      } else if (event.control_error) {
        it->second.pending->fail(
            std::make_exception_ptr(SDKError(ErrorDomain::signaling, *event.control_error,
                                             event.error, false, event.control_reason)));
      } else {
        if (it->second.applied) {
          it->second.applied();
        }
        it->second.pending->succeed();
      }
      erase_control(event.operation_id);
      return;
    }
    if (event.kind == TransportEventKind::statistics) {
      if (event.binding.epoch != epoch || !event.statistics) {
        return;
      }
      RoomEvent notification{RoomEventKind::statistics, {}, {}, epoch, {}};
      notification.statistics = std::move(event.statistics);
      emit(std::move(notification));
      return;
    }
    if (event.kind == TransportEventKind::subscription_permission_changed) {
      if (event.binding.epoch != epoch || !event.permission ||
          connection == ConnectionState::closed || connection == ConnectionState::failed) {
        return;
      }
      RoomEvent notification{RoomEventKind::subscription_permission_changed, {}, {}, epoch, {}};
      notification.permission = std::move(event.permission);
      emit(std::move(notification));
      return;
    }
    if (event.kind == TransportEventKind::session_changed) {
      if (!event.session || event.binding.epoch != epoch || event.session->epoch != epoch ||
          connection == ConnectionState::closed || connection == ConnectionState::failed) {
        return;
      }
      {
        if (status.session && status.session->revision >= event.session->revision) {
          return;
        }
        status.session = std::move(event.session);
      }
      emit({RoomEventKind::session_changed, {}, {}, epoch, {}});
      return;
    }
    if (event.kind == TransportEventKind::remote_track_added ||
        event.kind == TransportEventKind::remote_track_removed) {
      if (event.binding.epoch != epoch || connection == ConnectionState::closed ||
          connection == ConnectionState::failed) {
        return;
      }
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
          event.kind == TransportEventKind::subscribed) {
        transport->remove(
            event.binding,
            event.binding.publication);  // compensate a cancelled/timed-out publication
      }
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
        if (connection != ConnectionState::connected) {
          return;
        }
        endpoint.ready(std::move(event.server_sid));
        emit_track({RoomEventKind::track_ready, event.binding.id, endpoint.descriptor, epoch, {}},
                   event.binding.publication);
        if (auto completion = std::exchange(entry.completion, {})) {
          completion({});
        }
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
        auto completion = std::move(entry.completion);
        RoomEvent notification{failed ? RoomEventKind::track_failed : RoomEventKind::track_removed,
                               event.binding.id, endpoint.descriptor, epoch, event.error};
        if (failed) {
          notification.failure = info;
        }
        emit_track(std::move(notification), event.binding.publication);
        erase_track(it);
        refresh();
        if (completion) {
          completion(failure);
        }
        break;
      }
      case TransportEventKind::frame:
        if (!endpoint.publication && event.frame) {
          endpoint.write(std::move(*event.frame));
        }
        break;
      case TransportEventKind::feedback:
        if (endpoint.publication && event.feedback) {
          endpoint.update_feedback(*event.feedback);
        }
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

  void pump(bool interrupted) noexcept {
    struct FlushCompletions {
      CompletionBatch& batch;

      ~FlushCompletions() {
        batch.flush();
      }
    } flush{pending_completions};

    ++controller_wakeups;
    const bool pending_protocol =
        interrupted &&
        (joining || !controls.empty() ||
         std::any_of(tracks.begin(), tracks.end(),
                     [](const auto& entry) { return bool(entry.second.completion); }) ||
         std::any_of(removals.begin(), removals.end(),
                     [](const auto& removal) { return !removal.waiting.empty(); }));
    if (pending_protocol) {
      stop_all(ConnectionState::closed, "RTC operation cancelled by leave/close",
               error("RTC operation cancelled by leave/close", ErrorCode::cancelled));
      return;
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
};
}  // namespace detail

RoomEngine::RoomEngine(TransportFactory factory, Hooks hooks)
    : state_(std::make_unique<detail::RoomEngineState>(std::move(factory), std::move(hooks))) {}

RoomEngine::~RoomEngine() = default;

void RoomEngine::join(RoomOptions options, Completed<void> completed) {
  auto operation = std::make_shared<detail::PendingOperation<void>>(state_->pending_completions,
                                                                    std::move(completed));
  auto pending = operation;
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
        if (options.subscription_permissions) {
          detail::validate_subscription_permissions(*options.subscription_permissions);
        }
        if (s->connection != ConnectionState::disconnected) {
          auto replacement = s->transport_factory();
          if (!replacement) {
            {
              pending->fail(detail::error("RTC transport factory returned null",
                                          ErrorCode::invalid_argument));
              return;
            }
          }
          s->hooks.attach_services(*replacement);
          auto services = replacement->services();
          s->transport = std::move(replacement);

          // Release old service state after unlocking; it may own executors.
          s->services.swap(services);
        }
        {
          s->hooks.reset_events(options.max_events);
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
  return state_->deferred(operation);
}

void RoomEngine::publish(TrackDescriptor descriptor,
                         Completed<std::shared_ptr<PublishedTrack>> completed) {
  auto operation = std::make_shared<detail::PendingOperation<std::shared_ptr<PublishedTrack>>>(
      state_->pending_completions, std::move(completed));
  auto pending = operation;
  auto* s = state_.get();
  s->run_request(
      [&] {
        auto endpoint = s->make_endpoint(std::move(descriptor), true);
        if (!endpoint) {
          pending->fail(
              detail::error("Join the RTC room before adding tracks", ErrorCode::invalid_state));
          return;
        }
        auto track = std::shared_ptr<PublishedTrack>(new PublishedTrack(endpoint, s->hooks.tracks));
        s->add(endpoint, [s, pending, track](auto failure) {
          if (failure) {
            pending->fail(failure);
          } else {
            pending->succeed(track);
          }
        });
      },
      operation);
  return state_->deferred(operation);
}

void RoomEngine::subscribe(TrackDescriptor descriptor,
                           Completed<std::shared_ptr<SubscribedTrack>> completed) {
  auto operation = std::make_shared<detail::PendingOperation<std::shared_ptr<SubscribedTrack>>>(
      state_->pending_completions, std::move(completed));
  auto pending = operation;
  auto* s = state_.get();
  s->run_request(
      [&] {
        auto endpoint = s->make_endpoint(std::move(descriptor), false);
        if (!endpoint) {
          pending->fail(
              detail::error("Join the RTC room before adding tracks", ErrorCode::invalid_state));
          return;
        }
        auto track =
            std::shared_ptr<SubscribedTrack>(new SubscribedTrack(endpoint, s->hooks.tracks));
        s->add(endpoint, [s, pending, track](auto failure) {
          if (failure) {
            pending->fail(failure);
          } else {
            pending->succeed(track);
          }
        });
      },
      operation);
  return state_->deferred(operation);
}

void RoomEngine::update_local_participant(ParticipantUpdate update, ControlOptions options,
                                          Completed<void> completed) {
  auto operation = std::make_shared<detail::PendingOperation<void>>(state_->pending_completions,
                                                                    std::move(completed));
  auto pending = operation;
  try {
    size_t bytes = update.name.size() + update.metadata.size();
    if (update.name.size() > 1024 || update.metadata.size() > 65536 ||
        update.attributes.size() > 128 || !livekit::sdk::detail::valid_utf8(update.name) ||
        !livekit::sdk::detail::valid_utf8(update.metadata) || options.timeout.count() < 0 ||
        options.timeout > std::chrono::minutes(5)) {
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::invalid_argument,
                     "Invalid RTC control arguments");
    }
    for (const auto& [key, value] : update.attributes) {
      if (key.empty() || key.size() > 256 || value.size() > 16384 ||
          !livekit::sdk::detail::valid_utf8(key) || !livekit::sdk::detail::valid_utf8(value)) {
        throw SDKError(ErrorDomain::lifecycle, ErrorCode::invalid_argument,
                       "Invalid RTC control arguments");
      }
      bytes += key.size() + value.size();
    }
    if (bytes > 65536) {
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::invalid_argument,
                     "Invalid RTC control arguments");
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
  return state_->deferred(operation);
}

void RoomEngine::set_subscription_permissions(SubscriptionPermissions policy,
                                              ControlOptions options, Completed<void> completed) {
  auto operation = std::make_shared<detail::PendingOperation<void>>(state_->pending_completions,
                                                                    std::move(completed));
  auto pending = operation;
  try {
    detail::validate_subscription_permissions(policy);
    if (options.timeout.count() < 0 || options.timeout > std::chrono::minutes(5)) {
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::invalid_argument,
                     "Invalid RTC control arguments");
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
  return state_->deferred(operation);
}

void RoomEngine::leave(Completed<void> completed) {
  auto operation = std::make_shared<detail::PendingOperation<void>>(state_->pending_completions,
                                                                    std::move(completed));
  auto pending = operation;
  auto* s = state_.get();
  s->run_request(
      [&] {
        if (s->connection != ConnectionState::closed) {
          s->stop_all(ConnectionState::closed, "RTC room left");
        }
        pending->succeed();
      },
      operation);
  // run_request has synchronously settled this local stop operation. Notification
  // draining belongs to the notification queue, never to this controller lock.
  return state_->deferred(operation);
}

void RoomEngine::unpublish(const TrackId& id, Completed<void> done) {
  state_->close_id(id, true, std::move(done));
}

void RoomEngine::unsubscribe(const TrackId& id, Completed<void> done) {
  state_->close_id(id, false, std::move(done));
}

void RoomEngine::set_muted(const TrackId& id, bool muted, ControlOptions options,
                           Completed<void> done, std::shared_ptr<detail::Endpoint> expected) {
  state_->set_muted(id, muted, options, std::move(done), std::move(expected));
}

void RoomEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand, ControlOptions options,
                                    Completed<void> done,
                                    std::shared_ptr<detail::Endpoint> expected) {
  state_->set_receive_demand(id, demand, options, std::move(done), std::move(expected));
}

void RoomEngine::close_track(std::shared_ptr<detail::Endpoint> endpoint, Completed<void> done) {
  state_->close_track(endpoint, std::move(done));
}

void RoomEngine::request_keyframe(std::shared_ptr<detail::Endpoint> endpoint,
                                  Completed<void> done) {
  state_->request_keyframe(endpoint, std::move(done));
}

void RoomEngine::poll(bool interrupted) {
  state_->pump(interrupted);
}

std::optional<SteadyClock::time_point> RoomEngine::next_deadline() const {
  return state_->next_deadline();
}

void RoomEngine::report_failure(std::shared_ptr<detail::Endpoint> endpoint,
                                std::exception_ptr error) {
  state_->report_failure(std::move(endpoint), std::move(error));
}

void RoomEngine::finish_removal(std::shared_ptr<detail::Endpoint> endpoint,
                                std::exception_ptr error) {
  state_->finish_removal(endpoint, std::move(error));
  state_->pending_completions.flush();
}

void RoomEngine::notification_failed(std::exception_ptr error) {
  if (state_->connection != ConnectionState::failed &&
      (state_->connection != ConnectionState::closed ||
       describe_failure(error)->code == ErrorCode::callback_failed)) {
    state_->stop_all(ConnectionState::failed, describe_failure(error)->message, error);
    state_->pending_completions.flush();
  }
}

RoomSnapshot RoomEngine::snapshot() const {
  return state_->status;
}

Result<std::shared_ptr<RoomServices>> RoomEngine::services() const {
  if (!state_->services) {
    return Result<std::shared_ptr<RoomServices>>::failure(std::make_shared<FailureInfo>(
        FailureInfo{next_failure_id(), ErrorDomain::lifecycle, ErrorCode::unsupported,
                    "This transport does not provide room services"}));
  }
  return Result<std::shared_ptr<RoomServices>>::success(state_->services);
}
}  // namespace livekit::sdk::rtc
