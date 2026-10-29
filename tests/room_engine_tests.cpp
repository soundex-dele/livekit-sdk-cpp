#include "rtc_event_mailbox.hpp"
#ifndef LIVEKIT_TEST_RTC_NODES
#include "utoolkit/task_queue/event.h"
#include "utoolkit/task_queue/task_queue.h"
#endif
#include "livekit/rtc/livekit.hpp"
#include "livekit/rtc/room_engine.hpp"
#include "backends/rtc/endpoint.hpp"
#include "backends/rtc/bounded_events.hpp"
#include "support/ready_set.hpp"
#include "support/deadline_index.hpp"
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#ifdef LIVEKIT_TEST_RTC_NODES
#include "livekit/rtc/graph_api.hpp"
#include "engines/rtc/pipeline_engine.hpp"
#endif

using livekit::sdk::AsyncResult;
using livekit::sdk::CompletionSource;
using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::detail;
using namespace std::chrono_literals;

namespace {
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

template <class T>
T get(std::future<T> future) {
  check(future.wait_for(2s) == std::future_status::ready, "Asynchronous operation hung");
  if constexpr (std::is_void_v<T>)
    future.get();
  else
    return future.get();
}

template <class T>
T get(AsyncResult<T> result) {
  check(result.wait_for(2s) == std::future_status::ready, "Asynchronous operation hung");
  return result.get();
}

template <class F>
void eventually(F predicate, const char* message) {
  const auto deadline = SteadyClock::now() + 2s;
  while (!predicate()) {
    if (SteadyClock::now() >= deadline)
      throw std::runtime_error(message);
    std::this_thread::sleep_for(1ms);
  }
}

TrackDescriptor video(std::string id, size_t frames = 4, size_t bytes = 1024) {
  TrackDescriptor d;
  d.id = std::move(id);
  d.format.width = 640;
  d.format.height = 360;
  d.queue = {frames, bytes, 2s};
  return d;
}

EncodedFrame frame(Epoch epoch, uint64_t timestamp, bool key = true, size_t size = 32) {
  std::vector<uint8_t> bytes(size, 42);
  bytes[0] = bytes[1] = bytes[2] = 0;
  bytes[3] = 1;
  bytes[4] = key ? 0x65 : 0x41;
  return {EncodedBuffer(std::move(bytes)), "", timestamp, 33ms, {}, epoch, 1, key};
}

struct Harness {
  std::mutex mutex;
  std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup;

  void signal() {
    if (wakeup) {
      wakeup->signal();
    }
  }
  std::vector<TransportEvent> events;
  std::vector<BindingToken> published, subscribed, removed, plis;

  struct Sent {
    BindingToken binding;
    EncodedFrame frame;
  };

  std::vector<Sent> sent;
  std::map<TrackId, size_t> send_attempts;
  std::vector<ParticipantUpdateRequest> updates;
  std::vector<std::pair<BindingToken, TrackDescriptor>> publication_descriptors;
  std::vector<MuteRequest> mutes;
  std::vector<ReceiveDemandRequest> demands;
  std::vector<SubscriptionPermissionsRequest> permissions;
  std::optional<SubscriptionPermissions> initial_permissions;
  std::vector<std::pair<BindingToken, TrackDescriptor>> subscription_descriptors;
  std::set<TrackId> blocked;
  std::set<TrackId> send_failures;
  bool confirm = true, connect = true;
  bool defer_removal = false;
  std::vector<std::shared_ptr<CompletionSource<void>>> removal_completions;
  int starts = 0, stops = 0;
  std::thread::id owner;

  void push(TransportEvent e) {
    std::lock_guard lock(mutex);
    events.push_back(std::move(e));
    signal();
  }
};

class FakeTransport : public RoomTransport {
 public:
  explicit FakeTransport(std::shared_ptr<Harness> harness) : h(std::move(harness)) {}

  BackendCapabilities capabilities() const override {
    BackendCapabilities result;
    result.participant_update_ack = supports_ack_;
    return result;
  }

  void start(const RoomOptions& options, Epoch) override {
    std::lock_guard lock(h->mutex);
    h->wakeup = wakeup_;
    const auto* connection = dynamic_cast<const LiveKitConnectionOptions*>(options.backend.get());
    supports_ack_ = connection && connection->signaling.signal_protocol >= 15;
    h->owner = std::this_thread::get_id();
    ++h->starts;
    h->initial_permissions = options.subscription_permissions;
    if (h->connect)
      h->events.push_back({TransportEventKind::connected});
    h->signal();
  }

  std::vector<TransportEvent> take_events() override {
    std::unique_lock lock(h->mutex);
    check(h->owner == std::this_thread::get_id(), "Transport accessed outside controller");
    auto result = std::move(h->events);
    h->events.clear();
    return result;
  }

  void publish(const BindingToken& b, const TrackDescriptor& descriptor) override {
    std::lock_guard lock(h->mutex);
    h->published.push_back(b);
    h->publication_descriptors.emplace_back(b, descriptor);
    if (h->confirm)
      h->events.push_back({TransportEventKind::published, b, "sid-" + b.id});
    h->signal();
  }

  void subscribe(const BindingToken& b, const TrackDescriptor& d,
                 std::shared_ptr<QueueBudget>) override {
    std::lock_guard lock(h->mutex);
    h->subscribed.push_back(b);
    h->subscription_descriptors.emplace_back(b, d);
    if (h->confirm)
      h->events.push_back({TransportEventKind::subscribed, b, "remote-" + b.id});
    h->signal();
  }

  AsyncResult<void> remove(const BindingToken& b, bool) override {
    std::lock_guard lock(h->mutex);
    h->removed.push_back(b);
    if (h->defer_removal) {
      auto completion = std::make_shared<CompletionSource<void>>();
      auto result = completion->get_result();
      h->removal_completions.push_back(std::move(completion));
      return result;
    }
    CompletionSource<void> done;
    done.set_value();
    return done.get_result();
  }

  SendResult send(const BindingToken& b, const EncodedFrame& f) override {
    std::lock_guard lock(h->mutex);
    ++h->send_attempts[b.id];
    if (h->send_failures.contains(b.id))
      throw std::runtime_error("Synthetic per-track send failure");
    if (h->blocked.contains(b.id))
      return SendResult::would_block;
    h->sent.push_back({b, f});
    QueueBudget::release_to_caller(h->sent.back().frame.buffer);
    return SendResult::sent;
  }

  void request_keyframe(const BindingToken& b) override {
    std::lock_guard lock(h->mutex);
    h->plis.push_back(b);
  }

  void update_participant(ParticipantUpdateRequest request) override {
    std::lock_guard lock(h->mutex);
    h->updates.push_back(std::move(request));
  }

  void set_muted(MuteRequest request) override {
    std::lock_guard lock(h->mutex);
    h->mutes.push_back(request);
    TransportEvent event{TransportEventKind::control_completed, request.binding};
    event.operation_id = request.id;
    h->events.push_back(std::move(event));
    h->signal();
  }

  void set_receive_demand(ReceiveDemandRequest request) override {
    std::lock_guard lock(h->mutex);
    h->demands.push_back(request);
  }

  void set_subscription_permissions(SubscriptionPermissionsRequest request) override {
    std::lock_guard lock(h->mutex);
    h->permissions.push_back(request);
  }

  void stop() noexcept override {
    std::lock_guard lock(h->mutex);
    ++h->stops;
  }

 private:
  std::shared_ptr<Harness> h;
  bool supports_ack_ = false;
};

LiveKitRoomOptions options(size_t budget = 4096) {
  LiveKitRoomOptions o;
  o.url = "ws://test.invalid";
  o.token = "synthetic";
  o.max_queued_bytes = budget;
  o.operation_timeout = 500ms;
  return o;
}

void indexed_work_contract() {
  using namespace livekit::sdk::detail;
  auto owner = std::make_shared<WakeEvent>();
  auto ready = std::make_shared<ReadySet<int>>(owner, 2);
  auto old = ready->watch(1);
  auto current = ready->watch(2);
  for (int i = 0; i < 10000; ++i) {
    old->signal();
  }
  check(ready->pending() == 1 && ready->take() == std::set<int>{1},
        "Producer bursts did not coalesce");
  bool limited = false;
  try {
    ready->watch(3);
  } catch (const std::length_error&) {
    limited = true;
  }
  check(limited, "Ready registrations were not bounded");
  ready->forget(1);
  old->signal();
  current->signal();
  check(ready->take() == std::set<int>{2}, "Retired producer revived stale work");
  const auto observed = owner->revision();
  current->signal();  // Arrives after drain but before wait.
  check(owner->revision() != observed && ready->take() == std::set<int>{2},
        "Drain-to-wait race lost producer work");
  std::weak_ptr retained = ready;
  ready.reset();
  old->signal();
  current->signal();
  check(retained.expired(), "Producer wake handle retained its owner");

  DeadlineIndex<int> deadlines;
  const auto now = SteadyClock::now();
  for (int i = 1; i <= 10000; ++i) {
    deadlines.set(1, now + std::chrono::microseconds(i));
  }
  deadlines.set(2, now + 20ms);
  check(deadlines.size() == 2 && deadlines.next() == now + 10ms,
        "Deadline updates accumulated stale entries");
  check(deadlines.take_due(now + 9ms).empty(), "Deadline fired early");
  check(deadlines.take_due(now + 10ms) == std::vector<int>{1}, "Due deadline lost");
  deadlines.erase(2);
  check(!deadlines.next() && deadlines.size() == 0, "Cancelled deadline retained");
}

void dirty_track_dispatch_contract() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  get(room.join(options()));
  std::vector<std::shared_ptr<PublishedTrack>> tracks;
  for (int i = 0; i < 128; ++i) {
    auto descriptor = video("load-" + std::to_string(i));
    descriptor.queue.max_age = 5s;
    tracks.push_back(get(room.publish(std::move(descriptor))));
  }
  eventually([&] { return room.snapshot().scheduled_deadlines == 0; },
             "Confirmed tracks retained operation deadlines");
  const auto baseline = room.snapshot().tracks_serviced;
  BindingToken blocked;
  {
    std::lock_guard lock(h->mutex);
    h->blocked.insert("load-0");
    blocked = h->published.front();
  }
  check(tracks.front()->try_write(frame(1, 1)) == WriteStatus::accepted, "Load frame rejected");
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->send_attempts["load-0"] == 1;
      },
      "Blocked track not serviced");
  const auto previous_wake = room.snapshot().controller_wakeups;
  {
    std::lock_guard lock(h->mutex);
    h->signal();
  }
  eventually([&] { return room.snapshot().controller_wakeups > previous_wake; },
             "Controller did not observe explicit wake");
  for (int i = 1; i <= 20; ++i) {
    check(tracks[i]->try_write(frame(1, 1)) == WriteStatus::accepted, "Sibling admission failed");
  }
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->sent.size() == 20;
      },
      "Blocked sender starved active siblings");
  eventually([&] { return room.snapshot().tracks_serviced >= baseline + 21; },
             "Work diagnostics did not include active tracks");
  check(room.snapshot().tracks_serviced == baseline + 21,
        "Unrelated wake or sibling media serviced idle/blocked tracks");
  {
    std::lock_guard lock(h->mutex);
    check(h->send_attempts["load-0"] == 1, "Blocked sender was polled without capacity event");
    h->blocked.erase("load-0");
  }
  h->push({TransportEventKind::writable, blocked});
  eventually([&] { return tracks.front()->snapshot().consumed == 1; },
             "Writable callback did not resume blocked sender");
  eventually([&] { return room.snapshot().scheduled_deadlines == 0; },
             "Drained tracks retained expiry deadlines");
  get(room.leave());
  check(room.snapshot().publications == 0 && room.snapshot().scheduled_deadlines == 0,
        "Room teardown retained track scheduling state");
}

void asynchronous_detachment() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  get(room.join(options()));
  auto original = get(room.publish(video("reused")));
  auto peer = get(room.publish(video("peer")));
  {
    std::lock_guard lock(h->mutex);
    h->defer_removal = true;
  }
  auto first = original->close();
  auto repeated = room.unpublish("reused");
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->removal_completions.size() == 1;
      },
      "Removal was not submitted");
  // This must finish while the first native detachment is deliberately blocked.
  get(peer->set_muted(true));
  check(first.wait_for(0ms) == std::future_status::timeout &&
            repeated.wait_for(0ms) == std::future_status::timeout,
        "Close completed before native detachment");
  auto replacement = get(room.publish(video("reused")));
  auto old_handle = original->close();
  get(peer->set_muted(false));  // Fence the repeated handle close on the controller.
  check(old_handle.wait_for(0ms) == std::future_status::timeout,
        "Old handle bypassed its outstanding detachment");
  {
    std::lock_guard lock(h->mutex);
    h->removal_completions.front()->set_value();
  }
  get(std::move(first));
  get(std::move(repeated));
  get(std::move(old_handle));
  check(replacement->snapshot().state == TrackState::ready,
        "Old detachment closed the replacement track");
  auto failed = peer->close();
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->removal_completions.size() == 2;
      },
      "Second removal was not submitted");
  {
    std::lock_guard lock(h->mutex);
    h->removal_completions.back()->set_exception(
        std::make_exception_ptr(std::runtime_error("detachment failed")));
  }
  bool rejected = false;
  try {
    get(std::move(failed));
  } catch (const std::runtime_error& error) {
    rejected = std::string(error.what()) == "detachment failed";
  }
  check(rejected, "Native detachment failure was lost");
  auto closing = replacement->close();
  get(room.leave());
  get(std::move(closing));  // stop() supplies the barrier for unfinished removals.
}

void bounded_event_admission() {
  auto verify = []<class Event, class Kind>(Kind statistics, Kind control) {
    std::deque<Event> queue;
    auto enqueue = [&](Event event) {
      return enqueue_event(queue, std::move(event), 2, statistics,
                           [](auto&, auto&) { return false; });
    };
    auto report = [&](int target, int64_t timestamp) {
      Event event{statistics};
      event.statistics =
          std::make_shared<TransportStatistics>(TransportStatistics{target, timestamp, "[]"});
      return event;
    };
    check(enqueue(report(0, 1)) == EventAdmission::queued, "Statistics not queued");
    check(enqueue(Event{control}) == EventAdmission::queued, "Control not queued");
    check(enqueue(report(0, 2)) == EventAdmission::coalesced &&
              queue.front().statistics->timestamp_us == 2,
          "Full queue did not retain newest target statistics");
    check(enqueue(report(1, 3)) == EventAdmission::dropped && queue.size() == 2,
          "Statistics displaced control events");
    check(enqueue(Event{control}) == EventAdmission::queued && queue.front().kind == control &&
              queue.back().kind == control,
          "Control did not evict statistics");
    check(enqueue(Event{control}) == EventAdmission::overflow && queue.size() == 2,
          "Control overflow was silent or changed queue capacity");
  };
  verify.operator()<RoomEvent>(RoomEventKind::statistics, RoomEventKind::connected);
  verify.operator()<TransportEvent>(TransportEventKind::statistics, TransportEventKind::connected);
}

void permission_controls() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  auto opts = options();
  opts.subscription_permissions = SubscriptionPermissions{false, {{"viewer", false, {"camera"}}}};
  get(room.join(opts));
  {
    std::lock_guard lock(h->mutex);
    check(h->initial_permissions && !h->initial_permissions->all_participants,
          "Initial permission lost");
  }
  auto update = room.set_subscription_permissions({false, {{"viewer", true, {}}}});
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->permissions.size() == 1;
      },
      "Policy not dispatched");
  check(update.wait_for(0ms) != std::future_status::ready,
        "Policy completed before native submission");
  SubscriptionPermissionsRequest request;
  {
    std::lock_guard lock(h->mutex);
    request = h->permissions.front();
  }
  TransportEvent result{TransportEventKind::control_completed, {{}, request.epoch}};
  result.operation_id = request.id;
  h->push(result);
  get(std::move(update));
  for (const auto& policy : std::vector<SubscriptionPermissions>{
           {true, {{"viewer", true, {}}}},
           {false, {{"", true, {}}}},
           {false, {{"viewer", true, {"camera"}}}},
           {false, {{"viewer", false, {"camera", "camera"}}}},
           {false, {{"viewer", true, {}}, {"viewer", true, {}}}}}) {
    bool failed = false;
    try {
      get(room.set_subscription_permissions(policy));
    } catch (const std::invalid_argument&) {
      failed = true;
    }
    check(failed, "Invalid subscription policy accepted");
  }
  std::stop_source stop;
  stop.request_stop();
  bool failed = false;
  try {
    get(room.set_subscription_permissions({}, {0ms, stop.get_token()}));
  } catch (const livekit::sdk::SDKError& e) {
    failed = e.code() == livekit::sdk::ErrorCode::cancelled;
  }
  check(failed, "Pre-dispatch permission cancellation lost");
  auto interrupted = room.set_subscription_permissions({});
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->permissions.size() == 2;
      },
      "Policy not dispatched");
  h->push({TransportEventKind::interrupted});
  h->push({TransportEventKind::connected});
  failed = false;
  try {
    get(std::move(interrupted));
  } catch (const livekit::sdk::SDKError& e) {
    failed = e.code() == livekit::sdk::ErrorCode::outcome_unknown;
  }
  check(failed, "Interrupted permission change falsely confirmed");
  eventually(
      [&] {
        return room.snapshot().epoch == 2 && room.snapshot().state == ConnectionState::connected;
      },
      "Rejoin missing");
  while (room_events.take_event(0ms)) {
  }
  TransportEvent stale{TransportEventKind::subscription_permission_changed, {{}, 1}};
  stale.permission = SubscriptionPermissionChange{"publisher", "old-track", false};
  h->push(stale);
  auto current = stale;
  current.binding.epoch = 2;
  current.permission->track_sid = "new-track";
  h->push(current);
  auto notification = room_events.take_event(1s);
  while (notification && !notification->permission)
    notification = room_events.take_event(1s);
  check(notification && notification->permission &&
            notification->permission->track_sid == "new-track" &&
            !notification->permission->allowed,
        "Permission observation leaked old epoch or lost payload");
}

void statistics_events() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  auto opts = options();
  opts.max_events = 2;
  opts.stats_interval = 200ms;
  get(room.join(opts));
  while (room_events.take_event(0ms)) {
  }
  const auto epoch = room.snapshot().epoch;
  {
    std::lock_guard lock(h->mutex);
    for (int i = 0; i < 100; ++i) {
      TransportEvent e{TransportEventKind::statistics, {{}, epoch}};
      e.statistics = std::make_shared<TransportStatistics>(TransportStatistics{i % 2, i, "[]"});
      h->events.push_back(std::move(e));
      h->signal();
    }
    // A control event must displace diagnostics, never overflow because of them.
    TransportEvent e{TransportEventKind::remote_track_added, {"remote", epoch}};
    e.descriptor = video("remote");
    h->events.push_back(e);
    h->signal();
    h->signal();
  }
  bool remote = false;
  int statistics = 0;
  const auto deadline = SteadyClock::now() + 2s;
  while (!remote && SteadyClock::now() < deadline) {
    if (auto e = room_events.take_event(20ms)) {
      check(e->kind != RoomEventKind::failed, "Statistics exhausted control queue");
      if (e->kind == RoomEventKind::statistics) {
        ++statistics;
        check(e->statistics && e->statistics->json == "[]", "Statistics payload lost");
      }
      if (e->kind == RoomEventKind::remote_track_added)
        remote = true;
    }
  }
  check(remote && room.snapshot().state == ConnectionState::connected,
        "Diagnostics prevented remote discovery");
  while (room_events.take_event(0ms)) {
  }
  TransportEvent stale{TransportEventKind::statistics, {{}, epoch - 1}};
  stale.statistics = std::make_shared<TransportStatistics>();
  h->push(stale);
  check(!room_events.take_event(30ms), "Retired session statistics escaped epoch filter");
  get(room.leave());
  auto bad = options();
  bad.stats_interval = 199ms;
  RoomEngine invalid([] { return std::make_unique<FakeTransport>(std::make_shared<Harness>()); });
  bool rejected = false;
  try {
    get(invalid.join(bad));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "Unbounded statistics frequency admitted");
}

void receive_demand_controls() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  get(room.join(options()));
  auto descriptor = video("receive");
  descriptor.receive_demand.disabled = true;
  auto a = get(room.subscribe(descriptor)), b = get(room.subscribe(video("sibling")));
  check(a->snapshot().receive_demand.disabled && !b->snapshot().receive_demand.disabled,
        "Initial demand not isolated");
  auto complete = [&](size_t index, std::optional<livekit::sdk::ErrorCode> failure = {}) {
    ReceiveDemandRequest request;
    eventually(
        [&] {
          std::lock_guard lock(h->mutex);
          return h->demands.size() > index;
        },
        "Demand not dispatched");
    {
      std::lock_guard lock(h->mutex);
      request = h->demands.at(index);
    }
    TransportEvent event{TransportEventKind::control_completed, request.binding};
    event.operation_id = request.id;
    event.control_error = failure;
    h->push(std::move(event));
  };
  ReceiveDemand wanted;
  wanted.width = 320;
  wanted.height = 180;
  wanted.fps = 15;
  wanted.priority = 1;
  auto update = a->set_demand(wanted);
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->demands.size() == 1;
      },
      "Missing demand command");
  check(update.wait_for(0ms) != std::future_status::ready && a->snapshot().receive_demand.disabled,
        "Demand completed before native submission");
  complete(0);
  get(std::move(update));
  check(a->snapshot().receive_demand == wanted && b->snapshot().receive_demand == ReceiveDemand{},
        "Demand changed sibling");
  check(a->descriptor().receive_demand.disabled, "Mutable demand changed immutable descriptor");
  auto rejected = a->set_demand({});
  complete(1, livekit::sdk::ErrorCode::request_failed);
  bool failed = false;
  try {
    get(std::move(rejected));
  } catch (const livekit::sdk::SDKError&) {
    failed = true;
  }
  check(failed && a->snapshot().receive_demand == wanted,
        "Rejected demand changed restoration intent");
  ReceiveDemand bad = wanted;
  bad.fps = 241;
  failed = false;
  try {
    get(a->set_demand(bad));
  } catch (const std::invalid_argument&) {
    failed = true;
  }
  check(failed, "Invalid demand accepted");
  bad = wanted;
  bad.width = 0;
  failed = false;
  try {
    get(a->set_demand(bad));
  } catch (const std::invalid_argument&) {
    failed = true;
  }
  check(failed, "Incomplete receive dimensions accepted");
  auto audio = video("audio");
  audio.kind = MediaKind::audio;
  audio.format = {};
  audio.format.codec = Codec::opus;
  audio.format.clock_rate = 48000;
  audio.format.channels = 1;
  auto audio_track = get(room.subscribe(audio));
  failed = false;
  try {
    get(audio_track->set_demand(wanted));
  } catch (const std::invalid_argument&) {
    failed = true;
  }
  check(failed, "Video demand accepted for audio");
  std::stop_source cancelled;
  cancelled.request_stop();
  failed = false;
  try {
    get(a->set_demand({}, {0ms, cancelled.get_token()}));
  } catch (const livekit::sdk::SDKError& e) {
    failed = e.code() == livekit::sdk::ErrorCode::cancelled;
  }
  check(failed, "Pre-dispatch cancellation lost");
  auto pending = a->set_demand({});
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->demands.size() == 3;
      },
      "Pending demand not dispatched");
  h->push({TransportEventKind::interrupted});
  h->push({TransportEventKind::connected});
  failed = false;
  try {
    get(std::move(pending));
  } catch (const livekit::sdk::SDKError& e) {
    failed = e.code() == livekit::sdk::ErrorCode::outcome_unknown;
  }
  check(failed, "Interrupted demand falsely confirmed");
  eventually([&] { return a->snapshot().epoch == 2 && a->snapshot().state == TrackState::ready; },
             "Demand rejoin missing");
  complete(2);  // The old completion must not change the restored intent.
  {
    std::lock_guard lock(h->mutex);
    check(std::any_of(h->subscription_descriptors.begin(), h->subscription_descriptors.end(),
                      [&](const auto& entry) {
                        return entry.first.id == "receive" && entry.first.epoch == 2 &&
                               entry.second.receive_demand == wanted;
                      }),
          "Rejoin did not use last submitted demand");
  }
  auto fresh = room.set_receive_demand("receive", wanted);
  complete(3);
  get(std::move(fresh));
  get(a->close());
  auto replacement = get(room.subscribe(video("receive")));
  failed = false;
  try {
    get(a->set_demand(wanted));
  } catch (const livekit::sdk::SDKError& e) {
    failed = e.code() == livekit::sdk::ErrorCode::stale_handle;
  }
  check(failed && replacement->snapshot().receive_demand == ReceiveDemand{},
        "Stale handle changed replacement demand");
}

void publication_sources() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  get(room.join(options()));
  for (auto source :
       {PublicationSource::unknown, PublicationSource::camera, PublicationSource::microphone,
        PublicationSource::screen, PublicationSource::screen_audio}) {
    auto d = video("source-" + std::to_string(static_cast<int>(source)));
    d.source = source;
    if (source == PublicationSource::microphone || source == PublicationSource::screen_audio) {
      d.kind = MediaKind::audio;
      d.format = {};
      d.format.codec = Codec::opus;
      d.format.clock_rate = 48000;
      d.format.channels = 2;
    }
    auto track = get(room.publish(d));
    check(track->descriptor().source == source, "Publication source changed at admission");
  }
  for (auto source : {PublicationSource::microphone, PublicationSource::screen_audio,
                      static_cast<PublicationSource>(99)}) {
    auto d = video("invalid-source");
    d.source = source;
    bool rejected = false;
    try {
      get(room.publish(d));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "Invalid video publication source accepted");
  }
  for (auto source : {PublicationSource::camera, PublicationSource::screen}) {
    auto d = video("invalid-source");
    d.source = source;
    d.kind = MediaKind::audio;
    d.format = {};
    d.format.codec = Codec::opus;
    d.format.clock_rate = 48000;
    d.format.channels = 1;
    bool rejected = false;
    try {
      get(room.publish(d));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "Invalid audio publication source accepted");
  }
  h->push({TransportEventKind::interrupted});
  h->push({TransportEventKind::connected});
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->publication_descriptors.size() == 10;
      },
      "Sources not restored after rejoin");
  std::lock_guard lock(h->mutex);
  for (const auto& [binding, d] : h->publication_descriptors)
    check(d.id == "source-" + std::to_string(static_cast<int>(d.source)),
          "Rejoin lost source intent");
}

void mute_contract() {
  {
    auto budget = std::make_shared<QueueBudget>(4096);
    Endpoint endpoint(video("gate"), {"gate", 1, 1, true}, budget);
    endpoint.ready("gate-sid");
    endpoint.set_muted(true);
    SenderFeedback feedback;
    feedback.epoch = 1;
    feedback.revision = 2;
    feedback.encodings[""] = {500000, true, false, 0};
    endpoint.update_feedback(feedback);
    check(endpoint.snapshot().muted && !endpoint.feedback().encodings.at("").active,
          "Native rates overrode local mute gate");
  }
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  get(room.join(options()));
  auto descriptor = video("muted");
  descriptor.muted = true;
  auto a = get(room.publish(descriptor)), b = get(room.publish(video("sibling")));
  check(a->snapshot().muted && a->try_write(frame(1, 1)) == WriteStatus::not_ready,
        "Initial mute admitted media");
  get(a->set_muted(false));
  {
    std::lock_guard lock(h->mutex);
    h->blocked.insert("muted");
  }
  check(a->try_write(frame(1, 10)) == WriteStatus::accepted, "Unmute did not admit keyframe");
  auto before_mute = frame(1, 20);
  before_mute.captured = SteadyClock::now();
  const auto generation = a->feedback().media_generation;
  get(room.set_muted("muted", true));
  check(a->snapshot().muted && a->snapshot().queued_frames == 0 &&
            !a->feedback().encodings.at("").active,
        "Mute retained queued payload or active feedback");
  SenderFeedback native;
  native.epoch = 1;
  native.revision = 100;
  native.encodings[""] = {1000000, true, false, 0};
  BindingToken binding;
  {
    std::lock_guard lock(h->mutex);
    binding = h->published.front();
  }
  TransportEvent rates{TransportEventKind::feedback, binding};
  rates.feedback = native;
  h->push(std::move(rates));
  get(a->set_muted(false));
  check(a->feedback().media_generation >= generation + 2,
        "Mute/unmute was invisible between graph polls");
  check(a->try_write(before_mute) == WriteStatus::not_ready,
        "Old codec output crossed mute boundary");
  check(a->try_write(frame(1, 20, false)) == WriteStatus::keyframe_required,
        "Unmute accepted dependent video");
  check(a->try_write(frame(1, 30)) == WriteStatus::accepted, "Unmute recovery keyframe rejected");
  check(b->try_write(frame(1, 1)) == WriteStatus::accepted, "Mute blocked sibling");
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return !h->sent.empty() && h->sent.back().binding.id == "sibling";
      },
      "Sibling did not transmit");
  get(a->set_muted(true));
  h->push({TransportEventKind::interrupted});
  h->push({TransportEventKind::connected});
  eventually([&] { return a->snapshot().epoch == 2 && a->snapshot().state == TrackState::ready; },
             "Muted publication did not rejoin");
  check(a->snapshot().muted && a->try_write(frame(2, 1)) == WriteStatus::not_ready,
        "Rejoin lost mute intent");
  {
    std::lock_guard lock(h->mutex);
    check(std::any_of(h->publication_descriptors.begin(), h->publication_descriptors.end(),
                      [](const auto& item) {
                        return item.first.id == "muted" && item.first.epoch == 2 &&
                               item.second.muted;
                      }),
          "Rejoin omitted muted from publication request");
    for (const auto& value : h->published)
      if (value.id == "muted" && value.epoch == 2)
        binding = value;
  }
  TransportEvent server{TransportEventKind::mute_changed, binding};
  server.muted = false;
  h->push(std::move(server));
  eventually([&] { return !a->snapshot().muted; }, "Server unmute did not reach publication gate");
  get(a->close());
  auto replacement = get(room.publish(video("muted")));
  bool rejected = false;
  try {
    get(a->set_muted(true));
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected && !replacement->snapshot().muted, "Old handle muted replacement publication");
}

template <class T>
concept HasEventPolling = requires(T& value) { value.next_event(); };
template <class T>
concept HasFailurePolling = requires(T& value) { value.next_failure(); };
static_assert(!HasEventPolling<RoomEngine>);

#ifndef LIVEKIT_TEST_RTC_NODES
void task_queue_wakeup_contract() {
  vi::Event request, response;
  std::atomic<bool> timed_out{false};
  std::jthread worker([&] {
    for (int i = 0; i < 2000; ++i) {
      if (!request.wait(1000)) {
        timed_out = true;
        return;
      }
      response.set();
    }
  });
  for (int i = 0; i < 2000; ++i) {
    request.set();
    if (!response.wait(1000)) {
      timed_out = true;
      break;
    }
  }
  worker.join();
  check(!timed_out, "Task queue event lost a wakeup");
  request.set();
  request.reset();
  check(!request.wait(0), "Event reset retained a stale wakeup");
  auto queue = vi::TaskQueue::create("rtc.test.queue");
  std::promise<void> delayed;
  auto done = delayed.get_future();
  queue->postDelayedTask([&] { queue->postSyncTask([&] { delayed.set_value(); }); }, 10);
  get(std::move(done));
}
#endif

void room_callback_contract() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  std::promise<void> entered, release;
  auto entered_future = entered.get_future();
  auto released = release.get_future().share();
  std::atomic<int> callbacks{0};
  const auto caller = std::this_thread::get_id();
  room.set_event_handler([&](const RoomEvent& event) {
    ++callbacks;
    check(std::this_thread::get_id() != caller, "Room callback ran on the caller");
    if (event.kind == RoomEventKind::connected) {
      get(room.publish(video("callback-publication")));
    } else if (event.kind == RoomEventKind::track_ready) {
      entered.set_value();
      check(released.wait_for(2s) == std::future_status::ready, "Callback release timed out");
    }
  });
  get(room.join(options()));
  get(std::move(entered_future));
  auto removed = std::async(std::launch::async, [&] { room.set_event_handler({}); });
  check(removed.wait_for(30ms) == std::future_status::timeout,
        "Handler removal did not fence an in-flight callback");
  release.set_value();
  get(std::move(removed));
  const auto before = callbacks.load();
  get(room.leave());
  check(callbacks == before, "Callback ran after removal completed");
  room.set_event_handler([](const RoomEvent&) { throw std::runtime_error("callback failure"); });
  eventually([&] { return room.snapshot().state == ConnectionState::failed; },
             "Throwing callback escaped the task queue boundary");
  room.set_event_handler({});
}

void mute_event_overflow() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  auto opts = options();
  opts.max_events = 1;
  get(room.join(opts));
  check(bool(room_events.take_event(1s)), "Missing connected event");
  room.set_event_handler({});
  auto track = get(room.publish(video("event-capacity")));
  bool rejected = false;
  try {
    get(track->set_muted(true));
  } catch (const livekit::sdk::SDKError& e) {
    rejected = e.code() == livekit::sdk::ErrorCode::resource_exhausted;
  }
  check(rejected && !track->snapshot().muted, "Event overflow left failed mute intent");
  {
    std::lock_guard lock(h->mutex);
    check(h->mutes.empty(), "Event overflow dispatched unobservable native control");
  }
  room_events.attach(room);
  check(bool(room_events.take_event(1s)), "Missing publication event");
  get(track->set_muted(true));
  check(room.snapshot().state == ConnectionState::connected && track->snapshot().muted,
        "Rejected mute retained a double-completing promise");
}

void participant_controls() {
  using livekit::sdk::ErrorCode;
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  auto opts = options();
  opts.signal_protocol = 15;
  get(room.join(opts));
  const auto request = [&](size_t index) {
    eventually(
        [&] {
          std::lock_guard lock(h->mutex);
          return h->updates.size() > index;
        },
        "Participant control not dispatched");
    std::lock_guard lock(h->mutex);
    return h->updates.at(index);
  };
  const auto respond = [&](ParticipantUpdateRequest request, std::optional<ErrorCode> code = {}) {
    TransportEvent event{TransportEventKind::control_completed, {{}, request.epoch}};
    event.operation_id = request.id;
    event.control_error = code;
    event.error = "synthetic rejection";
    h->push(std::move(event));
  };
  const auto rejects = [&](AsyncResult<void> result, ErrorCode code) {
    bool rejected = false;
    try {
      get(std::move(result));
    } catch (const livekit::sdk::SDKError& e) {
      rejected = e.code() == code;
    }
    check(rejected, "Control failed with wrong outcome");
  };
  auto first = room.update_local_participant({"name", "metadata", {{"a", "b"}}});
  auto sent = request(0);
  check(first.wait_for(0ms) != std::future_status::ready && sent.update.attributes.at("a") == "b",
        "Control completed before ACK or lost fields");
  respond(sent);
  get(std::move(first));
  auto forbidden = room.update_local_participant({"denied", {}, {}});
  respond(request(1), ErrorCode::permission_denied);
  rejects(std::move(forbidden), ErrorCode::permission_denied);
  auto expires = room.update_local_participant({"expires", {}, {}}, {100ms});
  auto expired = request(2);
  rejects(std::move(expires), ErrorCode::outcome_unknown);
  respond(expired);  // Late ACK must not resolve a later operation.
  std::stop_source cancellation;
  auto cancelled =
      room.update_local_participant({"cancel", {}, {}}, {1s, cancellation.get_token()});
  request(3);
  cancellation.request_stop();
  rejects(std::move(cancelled), ErrorCode::outcome_unknown);
  rejects(room.update_local_participant({"cancel before", {}, {}}, {1s, cancellation.get_token()}),
          ErrorCode::cancelled);
  auto interrupted = room.update_local_participant({"old generation", {}, {}});
  auto old = request(4);
  h->push({TransportEventKind::interrupted});
  rejects(std::move(interrupted), ErrorCode::outcome_unknown);
  h->push({TransportEventKind::connected});
  eventually([&] { return room.snapshot().state == ConnectionState::connected; },
             "Control recovery did not connect");
  auto replacement = room.update_local_participant({"new generation", {}, {}});
  auto current = request(5);
  check(current.epoch != old.epoch && current.id > old.id, "Control identity reused after rejoin");
  auto stale = current;
  --stale.epoch;
  respond(stale);
  auto observation = std::make_shared<SessionInfo>();
  observation->epoch = current.epoch;
  observation->revision = 1;
  TransportEvent marker{TransportEventKind::session_changed, {{}, current.epoch}};
  marker.session = observation;
  h->push(std::move(marker));
  eventually([&] { return bool(room.snapshot().session); },
             "Control ACK ordering marker not processed");
  check(replacement.wait_for(0ms) != std::future_status::ready,
        "Wrong-epoch ACK resolved new operation");
  respond(current);
  get(std::move(replacement));
  std::vector<AsyncResult<void>> full;
  for (int i = 0; i < 128; ++i)
    full.push_back(room.update_local_participant({"queued", {}, {}}, {5s}));
  request(133);
  rejects(room.update_local_participant({"overflow", {}, {}}), ErrorCode::resource_exhausted);
  get(room.leave());
  for (auto& pending : full)
    rejects(std::move(pending), ErrorCode::outcome_unknown);
  rejects(room.update_local_participant({"closed", {}, {}}), ErrorCode::session_closed);
  bool invalid = false;
  try {
    get(room.update_local_participant({std::string("\xc0\x80", 2), {}, {}}));
  } catch (const std::invalid_argument&) {
    invalid = true;
  }
  check(invalid, "Invalid UTF8 update accepted");
}

void session_lifetime() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  auto opts = options();
  opts.max_events = 4;
  get(room.join(opts));
  const auto observe = [&](Epoch epoch, uint64_t revision) {
    auto session = std::make_shared<SessionInfo>();
    session->epoch = epoch;
    session->revision = revision;
    session->room.name = "observed";
    TransportEvent event{TransportEventKind::session_changed, {{}, epoch}};
    event.session = session;
    h->push(std::move(event));
  };
  observe(1, 3);
  eventually([&] { return bool(room.snapshot().session); }, "Session snapshot not installed");
  const auto retained = room.snapshot().session;
  for (uint64_t revision = 4; revision <= 100; ++revision)
    observe(1, revision);
  eventually(
      [&] {
        auto state = room.snapshot().session;
        return state && state->revision == 100;
      },
      "State hints exhausted event queue or lost latest observation");
  while (room_events.take_event(0ms)) {
  }
  observe(1, 2);
  h->push({TransportEventKind::interrupted});
  eventually([&] { return room.snapshot().epoch == 2; }, "Recovery did not advance epoch");
  check(!room.snapshot().session && retained->revision == 3,
        "Recovery retained live stale session or mutated history");
  observe(1, 100);
  observe(2, 1);
  h->push({TransportEventKind::connected});
  eventually([&] { return room.snapshot().state == ConnectionState::connected; },
             "Recovery connection missing");
  check(room.snapshot().session && room.snapshot().session->epoch == 2 &&
            room.snapshot().session->revision == 1,
        "Old session overwrote new generation");
  get(room.leave());
  check(!room.snapshot().session && retained->room.name == "observed",
        "Close snapshot lifetime broken");
}

void queue_contract() {
  auto budget = std::make_shared<QueueBudget>(96);
  auto a = std::make_shared<Endpoint>(video("a", 2, 64), BindingToken{"a", 1, 1}, budget);
  auto b = std::make_shared<Endpoint>(video("b", 2, 64), BindingToken{"b", 1, 2}, budget);
  a->ready("sid-a");
  b->ready("sid-b");
  check(a->write(frame(1, 1, false)) == WriteStatus::keyframe_required,
        "First video must be decodable");
  check(a->write(frame(1, 1)) == WriteStatus::accepted, "First keyframe");
  check(a->write(frame(1, 2, false)) == WriteStatus::accepted, "Dependent frame");
  check(b->write(frame(1, 1)) == WriteStatus::accepted && budget->used == 96,
        "Aggregate queue budget");
  check(b->write(frame(1, 2, false)) == WriteStatus::backpressure,
        "Total budget must reject allocation");
  check(b->snapshot().queued_frames == 0 && a->snapshot().queued_frames == 2,
        "Overload isolates reference chains by track");
  check(b->write(frame(1, 3, false)) == WriteStatus::keyframe_required,
        "Dropped dependency requires recovery");
  a->rebind(2);
  check(a->write(frame(1, 4)) == WriteStatus::stale_epoch && budget->used == 0,
        "Rebind discards old buffers");
  a->ready("new-sid");
  check(a->write(frame(2, 0)) == WriteStatus::accepted, "New epoch resets timestamp origin");
  a->finish(TrackState::closed);
  check(a->write(frame(2, 1)) == WriteStatus::closed && budget->used == 0,
        "Closed endpoint releases budget");
}

void queue_handoff() {
  auto budget = std::make_shared<QueueBudget>(32);
  Endpoint tx(video("tx", 1, 32), {"tx", 1, 1, true}, budget);
  Endpoint rx(video("rx", 1, 32), {"rx", 1, 2, false}, budget);
  tx.ready("tx");
  rx.ready("rx");
  check(tx.write(frame(1, 1)) == WriteStatus::accepted, "Queue admission failed");
  auto native = tx.front();
  tx.consumed(native->serial);
  check(tx.snapshot().queued_bytes == 0 && budget->used == 32,
        "Native handoff released room budget early");
  check(tx.snapshot().outstanding_frames == 1 && tx.snapshot().outstanding_bytes == 32,
        "Track snapshot omitted native-held payload");
  check(tx.write(frame(1, 2)) == WriteStatus::backpressure,
        "Endpoint plus native queue exceeded one-track budget");
  native.reset();
  check(budget->used == 0 && tx.snapshot().outstanding_frames == 0,
        "Native release leaked track reservation");
  // Receive begins with a native-owned room reservation; endpoint adoption
  // must not reserve the room again even when it has no free capacity.
  auto incoming = frame(1, 1);
  check(budget->charge(incoming.buffer), "Native receive room admission failed");
  check(rx.write(incoming) == WriteStatus::accepted,
        "Full budget deadlocked native-to-endpoint transfer");
  incoming.buffer = {};
  check(budget->used == 32, "Transfer double counted or released reservation");
  auto application = rx.read(0ms, {});
  check(application.status == ReadStatus::frame && budget->used == 0,
        "Application read retained room budget");
  check(application.frame->buffer.size() == 32, "Budget release invalidated application payload");

  auto large_room = std::make_shared<QueueBudget>(1024);
  Endpoint one_frame(video("one-frame", 1, 1024), {"one-frame", 1, 3, false}, large_room);
  one_frame.ready("one-frame");
  auto incoming_frame = frame(1, 1);
  check(one_frame.queue_budget()->charge(incoming_frame.buffer), "Native track admission failed");
  check(one_frame.write(incoming_frame) == WriteStatus::accepted,
        "Native track handoff double counted frame");
  incoming_frame.buffer = {};
  auto second_native = frame(1, 2);
  check(!one_frame.queue_budget()->charge(second_native.buffer),
        "Native receive plus endpoint exceeded shared frame count");
  check(one_frame.read(0ms, {}).status == ReadStatus::frame, "Combined receive budget lost frame");
  check(one_frame.queue_budget()->charge(second_native.buffer),
        "Read did not release native receive admission");
}

void feedback_admission() {
  auto budget = std::make_shared<QueueBudget>(1024);
  Endpoint endpoint(video("video"), {"video", 1, 1, true}, budget);
  endpoint.ready("sid");
  SenderFeedback feedback{1, 1, {{"", {100000, true, true, 1}}}};
  endpoint.update_feedback(feedback);
  check(endpoint.write(frame(1, 1)) == WriteStatus::accepted, "Keyframe after feedback rejected");
  ++feedback.revision;
  feedback.encodings[""].target_bitrate_bps = 200000;
  endpoint.update_feedback(feedback);
  check(endpoint.snapshot().queued_frames == 1, "Bitrate update erased queued recovery frame");
  ++feedback.revision;
  ++feedback.encodings[""].keyframe_requests;
  endpoint.update_feedback(feedback);
  check(endpoint.snapshot().queued_frames == 0, "New PLI failed to invalidate dependency chain");
  check(endpoint.write(frame(1, 2, false)) == WriteStatus::keyframe_required,
        "New PLI did not request recovery");
}

void multi_stream_and_recovery() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  get(room.join(options()));
  auto a = get(room.publish(video("camera-a")));
  auto b = get(room.publish(video("camera-b")));
  auto screen = get(room.publish(video("screen")));
  {
    std::lock_guard lock(h->mutex);
    h->blocked.insert("camera-a");
  }
  check(a->try_write(frame(1, 1)) == WriteStatus::accepted, "Queue blocked camera");
  check(b->try_write(frame(1, 1)) == WriteStatus::accepted, "Queue independent camera");
  check(screen->try_write(frame(1, 1)) == WriteStatus::accepted, "Queue independent screen");
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->sent.size() == 2;
      },
      "Blocked stream starved other streams");
  get(screen->close());
  check(room.snapshot().state == ConnectionState::connected &&
            a->snapshot().state == TrackState::ready,
        "Removing a track disconnected room");
  BindingToken old;
  {
    std::lock_guard lock(h->mutex);
    old = h->published[0];
  }
  h->push({TransportEventKind::interrupted});
  eventually([&] { return a->snapshot().state == TrackState::suspended; },
             "Transport interruption was not propagated");
  check(a->snapshot().queued_frames == 0 && a->try_write(frame(1, 2)) == WriteStatus::stale_epoch,
        "Old media survived recovery");
  get(b->close());  // deletion while offline must remove desired state
  h->push({TransportEventKind::connected});
  eventually([&] { return a->snapshot().state == TrackState::ready; },
             "Publication did not recover");
  check(a->snapshot().epoch == 2, "Restored endpoint has old epoch");
  {
    std::lock_guard lock(h->mutex);
    check(h->starts == 1 && h->published.size() == 4 && h->published.back().id == "camera-a",
          "Recovery republished deleted tracks or opened a room per track");
  }
  h->push({TransportEventKind::published, old, "late-old-sid"});
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return std::find(h->removed.begin(), h->removed.end(), old) != h->removed.end();
      },
      "Late ACK was not compensated");
  check(a->snapshot().server_sid == "sid-camera-a", "Late ACK replaced current binding");
  get(room.leave());
  check(a->snapshot().state == TrackState::closed && room.snapshot().queued_bytes == 0,
        "Leave did not close endpoints");
}

void receive_and_lifetime() {
  auto h = std::make_shared<Harness>();
  auto room = std::make_unique<RoomEngine>([h] { return std::make_unique<FakeTransport>(h); });
  get(room->join(options()));
  auto a = get(room->subscribe(video("remote-a")));
  auto b = get(room->subscribe(video("remote-b")));
  BindingToken ba, bb;
  {
    std::lock_guard lock(h->mutex);
    ba = h->subscribed[0];
    bb = h->subscribed[1];
  }
  TransportEvent ea{TransportEventKind::frame, ba};
  ea.frame = frame(ba.epoch, 9000);
  TransportEvent eb{TransportEventKind::frame, bb};
  eb.frame = frame(bb.epoch, 18000);
  h->push(std::move(ea));
  h->push(std::move(eb));
  auto ra = a->read(1s), rb = b->read(1s);
  check(ra.status == ReadStatus::frame && ra.frame->timestamp == 9000,
        "A received another track's media");
  check(rb.status == ReadStatus::frame && rb.frame->timestamp == 18000,
        "B received another track's media");
  // No media needs to arrive to send PLI.
  get(a->request_keyframe());
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return std::find(h->plis.begin(), h->plis.end(), ba) != h->plis.end();
      },
      "PLI waits for media arrival");
  std::stop_source stop;
  stop.request_stop();
  check(a->read(0ms, stop.get_token()).status == ReadStatus::cancelled,
        "Cancellation status missing");
  auto waiting = std::async(std::launch::async, [&] { return b->read(5s); });
  room.reset();
  check(get(std::move(waiting)).status == ReadStatus::closed,
        "Destroying room did not wake readers");
  check(ra.frame->buffer.size() == 32 && ra.frame->buffer.bytes()[4] == 0x65,
        "Read payload did not outlive room");
}

void timeout_and_reused_identity() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  auto opts = options();
  opts.operation_timeout = 50ms;
  get(room.join(opts));
  {
    std::lock_guard lock(h->mutex);
    h->confirm = false;
  }
  bool failed = false;
  try {
    get(room.publish(video("same")));
  } catch (const std::exception&) {
    failed = true;
  }
  check(failed && room.snapshot().state == ConnectionState::connected,
        "Publication timeout must not terminate room");
  BindingToken old;
  {
    std::lock_guard lock(h->mutex);
    old = h->published.front();
    h->confirm = true;
  }
  auto current = get(room.publish(video("same")));
  h->push({TransportEventKind::published, old, "late"});
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->removed.size() >= 2;
      },
      "Timeout ACK compensation missing");
  check(current->snapshot().server_sid != "late", "Reused id confused publication instances");
  get(current->close());
  auto newest = get(room.publish(video("same")));
  get(current->close());
  check(newest->snapshot().state == TrackState::ready && room.snapshot().publications == 1,
        "Old handle closed replacement track");
}

void cancel_pending_and_isolate_failure() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  get(room.join(options()));
  {
    std::lock_guard lock(h->mutex);
    h->confirm = false;
  }
  auto pending = room.publish(video("pending"));
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return !h->published.empty();
      },
      "Pending publication not submitted");
  get(room.unpublish("pending"));
  bool cancelled = false;
  try {
    get(std::move(pending));
  } catch (const std::exception&) {
    cancelled = true;
  }
  check(cancelled && room.snapshot().publications == 0, "Cancel did not erase pending intent");
  BindingToken pending_binding;
  {
    std::lock_guard lock(h->mutex);
    pending_binding = h->published.front();
    h->confirm = true;
  }
  h->push({TransportEventKind::published, pending_binding, "late-cancelled"});
  auto broken = get(room.publish(video("broken")));
  auto healthy = get(room.publish(video("healthy")));
  {
    std::lock_guard lock(h->mutex);
    h->send_failures.insert("broken");
  }
  check(broken->try_write(frame(1, 1)) == WriteStatus::accepted, "Failed-track frame not admitted");
  check(healthy->try_write(frame(1, 1)) == WriteStatus::accepted,
        "Healthy-track frame not admitted");
  eventually([&] { return broken->snapshot().state == TrackState::failed; },
             "Send failure was not surfaced");
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->sent.size() == 1 && h->sent.front().binding.id == "healthy";
      },
      "One track failure stopped sibling media");
  check(room.snapshot().state == ConnectionState::connected, "Track-local failure terminated room");
}

void endpoint_wait_contract() {
  auto budget = std::make_shared<QueueBudget>(4096);
  Endpoint endpoint(video("waiting"), {"waiting", 1, 1, false}, budget);
  endpoint.ready("sid");
  endpoint.rebind(2);
  std::promise<ReadResult> received;
  auto result = received.get_future();
  std::jthread reader([&](std::stop_token stop) { received.set_value(endpoint.wait_frame(stop)); });
  check(result.wait_for(20ms) == std::future_status::timeout,
        "Suspended receive did not remain blocked");
  endpoint.ready("new-sid");
  check(endpoint.write(frame(2, 42)) == WriteStatus::accepted, "Rebound frame rejected");
  auto item = get(std::move(result));
  check(item.status == ReadStatus::frame && item.frame->epoch == 2,
        "Blocking source lost wakeup after rebind");
  reader.join();

  std::promise<ReadResult> cancelled;
  auto cancellation = cancelled.get_future();
  std::jthread empty_reader(
      [&](std::stop_token stop) { cancelled.set_value(endpoint.wait_frame(stop)); });
  empty_reader.request_stop();
  check(get(std::move(cancellation)).status == ReadStatus::cancelled,
        "Stop failed to wake an empty receive source");

  Endpoint sender(video("feedback-wait"), {"feedback-wait", 1, 2, true}, budget);
  sender.ready("sid");
  const auto before = sender.feedback();
  std::promise<std::optional<SenderFeedback>> changed;
  auto change = changed.get_future();
  std::jthread feedback_reader([&](std::stop_token stop) {
    changed.set_value(sender.wait_feedback(before.epoch, before.revision, stop));
  });
  auto update = before;
  update.revision += 10;
  update.encodings.at("").target_bitrate_bps = 123456;
  sender.update_feedback(update);
  auto value = get(std::move(change));
  check(value && value->encodings.at("").target_bitrate_bps == 123456,
        "Transport feedback did not wake observers");
  feedback_reader.join();
  const auto cursor = sender.feedback();
  std::promise<std::optional<SenderFeedback>> closed;
  auto closing = closed.get_future();
  std::jthread closing_reader([&](std::stop_token stop) {
    closed.set_value(sender.wait_feedback(cursor.epoch, cursor.revision, stop));
  });
  sender.finish(TrackState::closed);
  check(!get(std::move(closing)), "Endpoint close retained a blocked feedback observer");
}

#ifdef LIVEKIT_TEST_RTC_NODES
class FrameConsumer : public graphflow::core::GraphNode {
 public:
  FrameConsumer() : GraphNode("test.consumer", nullptr) {}

  std::vector<EncodedFrame> frames() const {
    std::lock_guard lock(mutex_);
    return frames_;
  }

  std::thread::id delivery_thread() const {
    std::lock_guard lock(mutex_);
    return thread_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<EncodedFrame> frames_;
  std::thread::id thread_;

 protected:
  bool doInit() override {
    subscribe<nodes::EncodedFrameEvent>([this](const auto& event) {
      std::lock_guard lock(mutex_);
      thread_ = std::this_thread::get_id();
      frames_.push_back(event.frame);
    });
    return true;
  }
};

class FeedbackConsumer : public graphflow::core::GraphNode {
 public:
  FeedbackConsumer() : GraphNode("test.feedback", nullptr) {}

  std::vector<SenderFeedback> updates() const {
    std::lock_guard lock(mutex_);
    return updates_;
  }

  std::thread::id delivery_thread() const {
    std::lock_guard lock(mutex_);
    return thread_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<SenderFeedback> updates_;
  std::thread::id thread_;

 protected:
  bool doInit() override {
    subscribe<nodes::SenderFeedbackEvent>([this](const auto& event) {
      std::lock_guard lock(mutex_);
      thread_ = std::this_thread::get_id();
      updates_.push_back(event.feedback);
    });
    return true;
  }
};

void pipeline_contract() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  get(room.join(options()));
  auto a = get(room.publish(video("a")));
  auto b = get(room.publish(video("b")));
  auto receiver = get(room.subscribe(video("remote")));
  auto feedback = std::make_shared<FeedbackConsumer>();
  auto consumer = std::make_shared<FrameConsumer>();
  engine::PublishPipeline send_a(a, feedback), send_b(b);
  engine::SubscribePipeline receive(receiver, consumer);
  check(send_a.write(frame(1, 1)) == WriteStatus::not_ready, "Graph admitted media before start");
  send_a.start();
  send_b.start();
  receive.start();
  check(feedback->updates().size() == 1 && feedback->updates().back().epoch == 1,
        "Initial feedback must precede producer activation");
  check(send_a.write(frame(1, 1)) == WriteStatus::accepted, "Publish edge did not deliver media");
  check(send_b.write(frame(1, 1)) == WriteStatus::accepted,
        "Second publish edge did not deliver media");
  eventually([&] { return feedback->updates().size() >= 2; },
             "Feedback worker did not wake after revision changed");
  check(feedback->delivery_thread() != std::this_thread::get_id(),
        "Feedback was dispatched on the graph control thread");
  BindingToken remote;
  {
    std::lock_guard lock(h->mutex);
    remote = h->subscribed.front();
  }
  TransportEvent e{TransportEventKind::frame, remote};
  e.frame = frame(1, 3000);
  h->push(std::move(e));
  eventually([&] { return consumer->frames().size() == 1; },
             "Subscription source did not dispatch without control polling");
  check(consumer->delivery_thread() != std::this_thread::get_id(),
        "Receive source dispatched on the control thread");
  check(consumer->frames()[0].timestamp == 3000, "Subscription graph modified media timestamp");
  get(send_a.close());
  check(send_a.write(frame(1, 2)) == WriteStatus::closed, "Stopped graph accepted media");
  check(room.snapshot().state == ConnectionState::connected &&
            b->snapshot().state == TrackState::ready,
        "Stopping publication graph stopped room or sibling");
  check(send_b.write(frame(1, 2, false)) == WriteStatus::accepted,
        "Sibling graph did not continue");
  get(receive.close());
  get(send_b.close());
  check(room.snapshot().state == ConnectionState::connected,
        "Closing all graphs unexpectedly left room");
  auto late = get(room.publish(video("late-writer")));
  engine::PublishPipeline* pipeline = nullptr;
  bool quiesced = false;
  engine::PublishPipeline late_writer(late, engine::PublishGraphFactory([&](auto) {
                                        engine::TrackGraphSpec spec;
                                        spec.quiesce = [&] {
                                          // Models a producer finishing ingress while teardown joins it. Holding
                                          // the pipeline's ingress mutex across quiesce would deadlock here.
                                          quiesced =
                                              pipeline->write(frame(1, 1)) == WriteStatus::closed;
                                        };
                                        return spec;
                                      }));
  pipeline = &late_writer;
  late_writer.start();
  get(late_writer.close());
  check(quiesced, "Teardown failed to reject completing producer write");
}

class CallbackConsumer final : public graphflow::core::GraphNode {
 public:
  explicit CallbackConsumer(std::function<void()> callback)
      : GraphNode("callback.consumer", nullptr), callback_(std::move(callback)) {}

 private:
  bool doInit() override {
    subscribe<nodes::EncodedFrameEvent>([this](const auto&) { callback_(); });
    return true;
  }

  std::function<void()> callback_;
};

void source_worker_contract() {
  auto h = std::make_shared<Harness>();
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  get(room.join(options()));
  auto slow_track = get(room.subscribe(video("slow-source", 2)));
  auto fast_track = get(room.subscribe(video("fast-source")));
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<bool> entered = false, exited = false;
  auto slow = std::make_shared<CallbackConsumer>([&] {
    entered = true;
    released.wait_for(2s);  // Bounded even if an earlier assertion fails.
    exited = true;
  });
  auto fast = std::make_shared<FrameConsumer>();
  bool downstream_stopped = false;
  engine::SubscribePipeline slow_pipeline(
      slow_track, engine::SubscribeGraphFactory([&](auto source) {
        engine::TrackGraphSpec graph;
        graph.nodes = {slow};
        graph.connections = {
            engine::TrackGraphSpec::connect<nodes::EncodedFrameEvent>(source, slow)};
        graph.quiesce = [&] { downstream_stopped = exited.load(); };
        return graph;
      }));
  engine::SubscribePipeline fast_pipeline(fast_track, fast);
  slow_pipeline.start();
  fast_pipeline.start();
  BindingToken slow_binding, fast_binding;
  std::thread::id transport_thread;
  {
    std::lock_guard lock(h->mutex);
    slow_binding = h->subscribed[0];
    fast_binding = h->subscribed[1];
    transport_thread = h->owner;
  }
  const auto inject = [&](BindingToken binding, uint64_t timestamp) {
    TransportEvent event{TransportEventKind::frame, binding};
    event.frame = frame(binding.epoch, timestamp);
    h->push(std::move(event));
  };
  inject(slow_binding, 1);
  eventually([&] { return entered.load(); }, "Source worker never entered consumer");
  for (uint64_t i = 2; i < 20; ++i) {
    inject(slow_binding, i);
  }
  inject(fast_binding, 1);
  eventually([&] { return fast->frames().size() == 1; },
             "A slow source consumer blocked a sibling source or room transport");
  check(fast->delivery_thread() != transport_thread &&
            fast->delivery_thread() != std::this_thread::get_id(),
        "Graph data was dispatched on a transport/control thread");
  check(slow_track->snapshot().queued_frames <= 2, "Slow source escaped endpoint queue limits");
  std::jthread unblock([&] {
    std::this_thread::sleep_for(20ms);
    release.set_value();
  });
  get(slow_pipeline.close());
  check(exited && downstream_stopped, "Close did not join delivery before downstream teardown");
  check(slow_pipeline.node().delivered() == 1, "Close drained stale frames into stopped nodes");
  get(fast_pipeline.close());  // Must wake an idle source immediately.

  auto failed_track = get(room.subscribe(video("throwing-source")));
  auto throwing = std::make_shared<CallbackConsumer>([] {});
  std::promise<std::exception_ptr> reported;
  auto report = reported.get_future();
  engine::SubscribePipeline failed(
      failed_track,
      [throwing](auto source) {
        engine::TrackGraphSpec spec;
        spec.nodes = {throwing};
        spec.connections = {
            engine::TrackGraphSpec::connect<nodes::EncodedFrameEvent>(source, throwing)};
        return spec;
      },
      [&](auto error) { reported.set_value(error); });
  failed.start();
  BindingToken failed_binding;
  {
    std::lock_guard lock(h->mutex);
    failed_binding = h->subscribed.back();
  }
  TransportEvent failure{TransportEventKind::operation_failed, failed_binding};
  failure.error = "source receive failed";
  h->push(std::move(failure));
  const auto source_failure = get(std::move(report));
  check(bool(source_failure), "Source failure notification lost its cause");
  check(livekit::sdk::describe_failure(source_failure)->message == "source receive failed",
        "Source failure callback lost details");
  get(failed.close());
}

void engine_callback_contract() {
  auto h = std::make_shared<Harness>();
  engine::RtcEngine engine([h] { return std::make_unique<FakeTransport>(h); });
  std::promise<void> connected, failed;
  auto connected_future = connected.get_future();
  auto failed_future = failed.get_future();
  std::thread::id callback_thread;
  auto fault = std::make_shared<graphflow::core::GraphNode>("fault", nullptr);
  engine.set_callbacks({[&](const RoomEvent& event) {
                          if (event.kind == RoomEventKind::connected) {
                            callback_thread = std::this_thread::get_id();
                            get(engine.publish(video("callback-control")));
                            connected.set_value();
                          }
                        },
                        [&](const engine::RtcEngine::GraphFailure& failure) {
                          check(std::this_thread::get_id() == callback_thread,
                                "Engine notifications did not share a serial queue");
                          check(failure.id == "failed-graph" && failure.publication &&
                                    failure.message == "node failure",
                                "Graph failure callback lost details");
                          get(engine.unpublish("callback-control"));
                          engine.set_callbacks({});  // Self-removal must not wait on its own queue.
                          failed.set_value();
                        }});
  get(engine.join(options()));
  get(std::move(connected_future));
  check(callback_thread != std::this_thread::get_id(), "Callback ran on caller thread");
  get(engine.graph().publish_graph(video("failed-graph"), [&](auto) {
    engine::TrackGraphSpec spec;
    spec.nodes = {fault};
    return spec;
  }));
  fault->reportFailure(std::make_exception_ptr(std::runtime_error("node failure")));
  get(std::move(failed_future));
  engine.set_callbacks({});
  get(engine.close());
}

void active_failure_correlation_contract() {
  auto h = std::make_shared<Harness>();
  engine::RtcEngine engine([h] { return std::make_unique<FakeTransport>(h); });
  std::promise<std::shared_ptr<const livekit::sdk::FailureInfo>> graph_report, room_report;
  auto graph_result = graph_report.get_future(), room_result = room_report.get_future();
  std::atomic<size_t> notifications{0};
  engine.set_callbacks({[&](const RoomEvent& event) {
                          if (event.kind == RoomEventKind::track_failed &&
                              event.track_id == "fault") {
                            room_report.set_value(event.failure);
                          }
                        },
                        [&](const engine::RtcEngine::GraphFailure& failure) {
                          if (++notifications == 1) {
                            graph_report.set_value(failure.failure);
                          }
                        }});
  get(engine.join(options()));
  auto node = std::make_shared<graphflow::core::GraphNode>("codec.failure", nullptr);
  std::shared_ptr<PublishedTrack> endpoint;
  get(engine.graph().publish_graph(video("fault"), [&](auto sink) {
    endpoint = sink->track();
    engine::TrackGraphSpec spec;
    spec.nodes = {node};
    return spec;
  }));
  {
    std::lock_guard lock(h->mutex);
    h->defer_removal = true;
  }
  const auto cause = std::make_exception_ptr(std::runtime_error("asynchronous codec failure"));
  std::jthread reporter([&] {
    node->reportFailure(cause);
    node->reportFailure(cause);
  });
  const auto graph = get(std::move(graph_result));
  const auto room = get(std::move(room_result));
  const auto snapshot = endpoint->snapshot();
  check(graph && room && snapshot.failure && graph->id == room->id &&
            graph->id == snapshot.failure->id && graph->context.node == "codec.failure" &&
            snapshot.state == TrackState::failed && notifications == 1,
        "Failure was duplicated or lost correlation across graph, room and track");
  auto closed = endpoint->close();
  // A later room operation fences close admission without releasing the native
  // detachment. Source failure must not bypass the normal teardown barrier.
  get(engine.publish(video("failure-fence")));
  check(closed.wait_for(0ms) == std::future_status::timeout,
        "Failed graph close bypassed its native detachment barrier");
  {
    std::lock_guard lock(h->mutex);
    check(h->removal_completions.size() == 1, "Failed graph detached more than once");
    h->removal_completions.front()->set_value();
    h->defer_removal = false;
  }
  get(std::move(closed));

  auto old_node = std::make_shared<graphflow::core::GraphNode>("old.node", nullptr);
  std::shared_ptr<PublishedTrack> old_endpoint;
  get(engine.graph().publish_graph(video("reused-fault"), [&](auto sink) {
    old_endpoint = sink->track();
    engine::TrackGraphSpec spec;
    spec.nodes = {old_node};
    return spec;
  }));
  get(engine.unpublish("reused-fault"));
  get(engine.publish(video("reused-fault")));
  old_node->reportFailure(cause);
  get(engine.set_muted("reused-fault", false));  // Fences the stale owner notification.
  check(engine.publication("reused-fault").state == TrackState::ready && notifications == 1,
        "Old graph failure destroyed or reported against a replacement track");
  get(engine.close());
  engine.set_callbacks({});
}

void engine_contract() {
  auto h = std::make_shared<Harness>();
  engine::RtcEngine engine([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox engine_events(engine);
  get(engine.join(options()));
  auto first = engine.publish(video("first"));
  auto second = engine.publish(video("second"));
  auto consumer = std::make_shared<FrameConsumer>();
  auto incoming = engine.graph().subscribe(video("incoming"), consumer);
  eventually(
      [&] {

        return first.wait_for(0ms) == std::future_status::ready &&
               second.wait_for(0ms) == std::future_status::ready &&
               incoming.wait_for(0ms) == std::future_status::ready;
      },
      "Engine did not complete asynchronous graph preparation");
  check(first.get() == "first" && second.get() == "second" && incoming.get() == "incoming",
        "Engine graph identity mismatch");
  check(engine.write("first", frame(engine.publication("first").epoch, 1)) == WriteStatus::accepted,
        "Engine source did not reach publish node");
  get(engine.unpublish("first"));
  check(engine.write("first", frame(1, 2)) == WriteStatus::closed,
        "Removed engine graph accepts input");
  check(engine.write("second", frame(1, 1)) == WriteStatus::accepted,
        "Removing graph affected other publication");
  {
    std::lock_guard lock(h->mutex);
    h->confirm = false;
  }
  auto cancelled = engine.publish(video("cancel"));
  get(engine.unpublish("cancel"));
  bool rejected = false;
  try {
    get(std::move(cancelled));
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "Pending engine graph was not cancelled");

  check(engine.write("cancel", frame(1, 1)) == WriteStatus::closed,
        "Cancelled graph was resurrected");
  // A server-side revoke removes only its graph and never replays the intent.
  BindingToken revoked;
  {
    std::lock_guard lock(h->mutex);
    for (const auto& binding : h->published)
      if (binding.id == "second")
        revoked = binding;
  }
  h->push({TransportEventKind::removed, revoked, {}, "Permission revoked"});
  eventually(
      [&] {

        return engine.write("second", frame(1, 2)) == WriteStatus::closed;
      },
      "Server revoke did not close publication graph");
  check(!engine_events.take_failure(), "Normal server removal became a graph processing failure");
  bool removal_observed = false;
  eventually(
      [&] {
        while (auto event = engine_events.take_event(0ms)) {
          if (event->kind == RoomEventKind::track_removed && event->track_id == "second") {
            removal_observed = event->descriptor && event->descriptor->id == "second";
          }
        }
        return removal_observed;
      },
      "Publication removal callback did not arrive");
  check(removal_observed, "Publication removal notification missing its identity");
  check(engine.room().snapshot().state == ConnectionState::connected &&
            engine.room().snapshot().subscriptions == 1,
        "Server revoke disturbed the shared room or subscription");
  get(engine.close());
  check(engine.room().snapshot().state == ConnectionState::closed,
        "Engine close did not stop room");
  std::lock_guard lock(h->mutex);
  check(h->starts == 1, "Engine created a connection per graph");
}

class GraphProducer : public graphflow::core::GraphNode {
 public:
  GraphProducer() : GraphNode("producer", nullptr) {}

  void emit(EncodedFrame frame) {
    publish(nodes::EncodedFrameEvent(std::move(frame)));
  }
};

class GraphRelay : public graphflow::core::GraphNode {
 public:
  GraphRelay() : GraphNode("relay", nullptr) {}

 protected:
  bool doInit() override {
    subscribe<nodes::EncodedFrameEvent>([this](const auto& value) { publish(value); });
    return true;
  }
};

void composed_graph_contract() {
  auto h = std::make_shared<Harness>();
  engine::RtcEngine engine([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox engine_events(engine);
  get(engine.join(options()));
  std::atomic<int> activations{0}, quiescences{0};
  bool teardown_while_running = false;
  std::shared_ptr<GraphProducer> producer;
  auto publication = engine.graph().publish_graph(video("composed"), [&](auto sink) {
    check(sink->track()->snapshot().state == TrackState::ready, "Graph factory ran before ACK");
    producer = std::make_shared<GraphProducer>();
    auto relay = std::make_shared<GraphRelay>();
    engine::TrackGraphSpec spec;
    spec.nodes = {relay, producer};
    spec.connections = {engine::TrackGraphSpec::connect<nodes::EncodedFrameEvent>(producer, relay),
                        engine::TrackGraphSpec::connect<nodes::EncodedFrameEvent>(relay, sink)};
    spec.activate = [&, relay, sink] {
      check(producer->state() == graphflow::core::LifecycleState::Running &&
                relay->state() == graphflow::core::LifecycleState::Running &&
                sink->state() == graphflow::core::LifecycleState::Running,
            "Producer activated before full graph start");
      ++activations;
      producer->emit(frame(sink->track()->snapshot().epoch, 1));
    };
    spec.quiesce = [&, sink] {
      ++quiescences;
      teardown_while_running = producer->state() == graphflow::core::LifecycleState::Running &&
                               sink->track()->snapshot().state == TrackState::ready;
    };
    return spec;
  });
  eventually(
      [&] {

        return publication.wait_for(0ms) == std::future_status::ready;
      },
      "Composed graph never started");
  check(publication.get() == "composed" && activations == 1, "Composed graph lifecycle failed");
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return !h->sent.empty();
      },
      "Composed graph edges did not reach transport");
  auto consumer = std::make_shared<FrameConsumer>();
  auto subscription = engine.graph().subscribe_graph(video("receive-composed"), [consumer](
                                                                                    auto source) {
    auto relay = std::make_shared<GraphRelay>();
    engine::TrackGraphSpec spec;
    spec.nodes = {consumer, relay};
    spec.connections = {engine::TrackGraphSpec::connect<nodes::EncodedFrameEvent>(source, relay),
                        engine::TrackGraphSpec::connect<nodes::EncodedFrameEvent>(relay, consumer)};
    return spec;
  });
  eventually(
      [&] {

        return subscription.wait_for(0ms) == std::future_status::ready;
      },
      "Composed subscription never started");
  subscription.get();
  BindingToken binding;
  {
    std::lock_guard lock(h->mutex);
    binding = h->subscribed.back();
  }
  TransportEvent e{TransportEventKind::frame, binding};
  e.frame = frame(1, 3000);
  h->push(std::move(e));
  eventually([&] { return !consumer->frames().empty(); }, "Receive composition dropped media");
  get(engine.unpublish("composed"));
  check(quiescences == 1 && teardown_while_running, "Graph stop preceded producer quiescence");
  check(engine.room().snapshot().state == ConnectionState::connected,
        "Graph close left shared room");
  auto failing = engine.graph().publish_graph(video("failure"), [&](auto) {
    engine::TrackGraphSpec spec;
    spec.activate = [] { throw std::runtime_error("source activation failed"); };
    spec.quiesce = [&] { ++quiescences; };
    return spec;
  });
  eventually(
      [&] {

        return failing.wait_for(0ms) == std::future_status::ready;
      },
      "Activation failure unresolved");
  bool rejected = false;
  try {
    failing.get();
  } catch (...) {
    rejected = true;
  }
  check(rejected && quiescences == 2, "Partial activation did not quiesce exactly once");
  eventually([&] { return engine.room().snapshot().publications == 0; },
             "Failed graph retained native endpoint");
  get(engine.close());
}
#endif
// A new adapter/epoch prevents retained endpoints and delayed notifications from
// changing a later join that reuses the same application track id.
void room_reuse_contract() {
  auto h = std::make_shared<Harness>();
  int adapters = 0;
  RoomEngine room([&] {
    ++adapters;
    return std::make_unique<FakeTransport>(h);
  });
  get(room.join(options()));
  auto old_track = get(room.publish(video("camera")));
  BindingToken old_binding;
  {
    std::lock_guard lock(h->mutex);
    old_binding = h->published.back();
  }
  const auto old_epoch = room.snapshot().epoch;
  get(room.leave());
  get(room.join(options()));
  check(adapters == 2 && room.snapshot().epoch > old_epoch,
        "Rejoin reused an adapter or connection epoch");
  auto current = get(room.publish(video("camera")));
  get(old_track->close());
  h->push({TransportEventKind::removed, old_binding});
  check(current->try_write(frame(current->snapshot().epoch, 1)) == WriteStatus::accepted,
        "Old endpoint closed the new publication");
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return !h->sent.empty();
      },
      "Rejoined publication did not send");
  check(current->snapshot().state == TrackState::ready, "Stale event removed current endpoint");
  get(room.leave());
}

#ifdef LIVEKIT_TEST_RTC_NODES
template <class T>
concept CanLeave = requires(T& value) { value.leave(); };
template <class T>
concept CanPublish = requires(T& value) { value.publish(TrackDescriptor{}); };
static_assert(!CanLeave<RoomView> && !CanPublish<RoomView>);
template <class T>
concept HasPoll = requires(T& value) { value.poll(); };
static_assert(!HasEventPolling<engine::RtcEngine> && !HasFailurePolling<engine::RtcEngine>);
static_assert(!HasPoll<engine::RtcEngine> && !HasPoll<engine::RtcPipelineEngine> &&
              !HasPoll<engine::SubscribePipeline> && !HasPoll<engine::PublishPipeline> &&
              !HasPoll<nodes::RtcSubscribeNode>);

void facade_lifecycle_contract() {
  auto h = std::make_shared<Harness>();
  engine::RtcEngine rtc([h] { return std::make_unique<FakeTransport>(h); });
  check(!rtc.media_enabled(), "Encoded facade unexpectedly installed media");
  bool unsupported = false;
  try {
    rtc.publish_media({});
  } catch (const livekit::sdk::SDKError& error) {
    unsupported = error.code() == livekit::sdk::ErrorCode::unsupported;
  }
  check(unsupported, "Missing media module was not reported");
  get(rtc.join(options()));
  {
    std::lock_guard lock(h->mutex);
    h->confirm = false;
  }
  auto pending = rtc.publish(video("pending"));
  get(rtc.leave());
  bool cancelled = false;
  try {
    get(std::move(pending));
  } catch (const std::exception&) {
    cancelled = true;
  }
  check(cancelled, "Leave did not settle pending graph creation");
  get(rtc.join(options()));
  {
    std::lock_guard lock(h->mutex);
    h->confirm = true;
  }
  auto active = rtc.publish(video("pending"));
  eventually(
      [&] {

        return active.wait_for(0ms) == std::future_status::ready;
      },
      "Facade failed to publish after rejoin");
  check(active.get() == "pending", "Rejoined graph has wrong identity");
  std::exception_ptr cross_thread_failure;
  std::thread other([&] {
    try {
      get(rtc.set_muted("pending", true));
    } catch (...) {
      cross_thread_failure = std::current_exception();
    }
  });
  other.join();
  if (cross_thread_failure) {
    std::rethrow_exception(cross_thread_failure);
  }
  check(rtc.publication("pending").muted,
        "Facade did not serialize cross-thread control on its task queue");
  get(rtc.close());
  get(rtc.close());
  check(rtc.write("pending", frame(2, 1)) == WriteStatus::closed,
        "Terminal facade still accepts frames");
  bool closed = false;
  try {
    rtc.join(options());
  } catch (const livekit::sdk::SDKError& error) {
    closed = error.code() == livekit::sdk::ErrorCode::session_closed;
  }
  check(closed, "Terminal close allowed another join");
}
#endif

void typed_join_failure() {
  auto h = std::make_shared<Harness>();
  h->connect = false;
  RoomEngine room([h] { return std::make_unique<FakeTransport>(h); });
  livekit::sdk::app_support::RtcEventMailbox room_events(room);
  LiveKitRoomOptions options;
  options.url = "ws://test";
  options.token = "test";
  auto joining = room.join(options);
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return h->starts == 1;
      },
      "Join not started");
  TransportEvent event{TransportEventKind::terminal_error};
  event.error = "Access denied";
  event.failure = std::make_exception_ptr(
      livekit::sdk::SDKError(livekit::sdk::ErrorDomain::signaling,
                             livekit::sdk::ErrorCode::permission_denied, event.error, false, 403));
  h->push(std::move(event));
  bool rejected = false;
  try {
    get(std::move(joining));
  } catch (const livekit::sdk::SDKError& error) {
    rejected = error.domain() == livekit::sdk::ErrorDomain::signaling &&
               error.code() == livekit::sdk::ErrorCode::permission_denied &&
               error.backend_reason() == 403 && !error.retryable();
  }
  check(rejected, "Transport join failure lost typed cause");
  check(room.snapshot().state == ConnectionState::failed, "Terminal failure not visible");
}

}  // namespace

int main() {
  try {
    asynchronous_detachment();
    bounded_event_admission();
#ifdef LIVEKIT_TEST_RTC_NODES
    std::cout << "facade lifecycle" << std::endl;
    facade_lifecycle_contract();
    std::cout << "pipeline lifecycle" << std::endl;
    pipeline_contract();
    std::cout << "source workers" << std::endl;
    source_worker_contract();
    std::cout << "engine lifecycle" << std::endl;
    engine_contract();
    std::cout << "engine callbacks" << std::endl;
    engine_callback_contract();
    active_failure_correlation_contract();
    std::cout << "composed graphs" << std::endl;
    composed_graph_contract();
    std::cout << "RTC graph pipeline contracts passed\n";
#else
    indexed_work_contract();
    dirty_track_dispatch_contract();
    task_queue_wakeup_contract();
    room_callback_contract();
    endpoint_wait_contract();
    room_reuse_contract();
    typed_join_failure();
    statistics_events();
    permission_controls();
    receive_demand_controls();
    publication_sources();
    mute_contract();
    mute_event_overflow();
    participant_controls();
    session_lifetime();
    queue_contract();
    queue_handoff();
    feedback_admission();
    multi_stream_and_recovery();
    receive_and_lifetime();
    timeout_and_reused_identity();
    cancel_pending_and_isolate_failure();
    std::cout << "RTC backend contracts passed\n";
#endif
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
