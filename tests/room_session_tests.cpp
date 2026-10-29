#include "livekit/rtc/room_session.hpp"
#include "backends/rtc/endpoint.hpp"
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#ifdef LIVEKIT_TEST_RTC_NODES
#include "livekit/rtc/engine.hpp"
#endif

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
  std::condition_variable_any ready;
  std::vector<TransportEvent> events;
  std::vector<BindingToken> published, subscribed, removed, plis;

  struct Sent {
    BindingToken binding;
    EncodedFrame frame;
  };

  std::vector<Sent> sent;
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
  int starts = 0, stops = 0;
  std::thread::id owner;

  void push(TransportEvent e) {
    std::lock_guard lock(mutex);
    events.push_back(std::move(e));
    ready.notify_all();
  }
};

class FakeTransport : public RoomTransport {
 public:
  explicit FakeTransport(std::shared_ptr<Harness> harness) : h(std::move(harness)) {}

  void start(const RoomOptions& options) override {
    std::lock_guard lock(h->mutex);
    h->owner = std::this_thread::get_id();
    ++h->starts;
    h->initial_permissions = options.subscription_permissions;
    if (h->connect)
      h->events.push_back({TransportEventKind::connected});
  }

  std::vector<TransportEvent> poll(std::chrono::milliseconds wait, std::stop_token stop) override {
    std::unique_lock lock(h->mutex);
    check(h->owner == std::this_thread::get_id(), "Transport accessed outside controller");
    h->ready.wait_for(lock, stop, wait, [&] { return !h->events.empty(); });
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
  }

  void subscribe(const BindingToken& b, const TrackDescriptor& d,
                 std::shared_ptr<QueueBudget>) override {
    std::lock_guard lock(h->mutex);
    h->subscribed.push_back(b);
    h->subscription_descriptors.emplace_back(b, d);
    if (h->confirm)
      h->events.push_back({TransportEventKind::subscribed, b, "remote-" + b.id});
  }

  void remove(const BindingToken& b, bool) override {
    std::lock_guard lock(h->mutex);
    h->removed.push_back(b);
  }

  SendResult send(const BindingToken& b, const EncodedFrame& f) override {
    std::lock_guard lock(h->mutex);
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
};

RoomOptions options(size_t budget = 4096) {
  RoomOptions o;
  o.url = "ws://test.invalid";
  o.token = "synthetic";
  o.max_queued_bytes = budget;
  o.operation_timeout = 500ms;
  return o;
}

void permission_controls() {
  auto h = std::make_shared<Harness>();
  RoomSession room(std::make_unique<FakeTransport>(h));
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
  while (room.next_event(0ms)) {
  }
  TransportEvent stale{TransportEventKind::subscription_permission_changed, {{}, 1}};
  stale.permission = SubscriptionPermissionChange{"publisher", "old-track", false};
  h->push(stale);
  auto current = stale;
  current.binding.epoch = 2;
  current.permission->track_sid = "new-track";
  h->push(current);
  auto notification = room.next_event(1s);
  while (notification && !notification->permission)
    notification = room.next_event(1s);
  check(notification && notification->permission &&
            notification->permission->track_sid == "new-track" &&
            !notification->permission->allowed,
        "Permission observation leaked old epoch or lost payload");
}

void statistics_events() {
  auto h = std::make_shared<Harness>();
  RoomSession room(std::make_unique<FakeTransport>(h));
  auto opts = options();
  opts.max_events = 2;
  opts.stats_interval = 200ms;
  get(room.join(opts));
  while (room.next_event(0ms)) {
  }
  const auto epoch = room.snapshot().epoch;
  {
    std::lock_guard lock(h->mutex);
    for (int i = 0; i < 100; ++i) {
      TransportEvent e{TransportEventKind::statistics, {{}, epoch}};
      e.statistics = std::make_shared<TransportStatistics>(TransportStatistics{i % 2, i, "[]"});
      h->events.push_back(std::move(e));
    }
    // A control event must displace diagnostics, never overflow because of them.
    TransportEvent e{TransportEventKind::remote_track_added, {"remote", epoch}};
    e.descriptor = video("remote");
    h->events.push_back(e);
    h->ready.notify_all();
  }
  bool remote = false;
  int statistics = 0;
  const auto deadline = SteadyClock::now() + 2s;
  while (!remote && SteadyClock::now() < deadline) {
    if (auto e = room.next_event(20ms)) {
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
  while (room.next_event(0ms)) {
  }
  TransportEvent stale{TransportEventKind::statistics, {{}, epoch - 1}};
  stale.statistics = std::make_shared<TransportStatistics>();
  h->push(stale);
  check(!room.next_event(30ms), "Retired session statistics escaped epoch filter");
  get(room.leave());
  auto bad = options();
  bad.stats_interval = 199ms;
  RoomSession invalid(std::make_unique<FakeTransport>(std::make_shared<Harness>()));
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
  RoomSession room(std::make_unique<FakeTransport>(h));
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
  RoomSession room(std::make_unique<FakeTransport>(h));
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
  RoomSession room(std::make_unique<FakeTransport>(h));
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

void mute_event_overflow() {
  auto h = std::make_shared<Harness>();
  RoomSession room(std::make_unique<FakeTransport>(h));
  auto opts = options();
  opts.max_events = 1;
  get(room.join(opts));
  check(bool(room.next_event(1s)), "Missing connected event");
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
  check(bool(room.next_event(1s)), "Missing publication event");
  get(track->set_muted(true));
  check(room.snapshot().state == ConnectionState::connected && track->snapshot().muted,
        "Rejected mute retained a double-completing promise");
}

void participant_controls() {
  using livekit::sdk::ErrorCode;
  auto h = std::make_shared<Harness>();
  RoomSession room(std::make_unique<FakeTransport>(h));
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
  const auto rejects = [&](std::future<void> result, ErrorCode code) {
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
  std::vector<std::future<void>> full;
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
  RoomSession room(std::make_unique<FakeTransport>(h));
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
  while (room.next_event(0ms)) {
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
  RoomSession room(std::make_unique<FakeTransport>(h));
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
  auto room = std::make_unique<RoomSession>(std::make_unique<FakeTransport>(h));
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
  RoomSession room(std::make_unique<FakeTransport>(h));
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
  RoomSession room(std::make_unique<FakeTransport>(h));
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
#ifdef LIVEKIT_TEST_RTC_NODES
class FrameConsumer : public graphflow::core::GraphNode {
 public:
  FrameConsumer() : GraphNode("test.consumer", nullptr) {}

  std::vector<EncodedFrame> frames;

 protected:
  bool doInit() override {
    subscribe<nodes::EncodedFrameEvent>(
        [this](const auto& event) { frames.push_back(event.frame); });
    return true;
  }
};

class FeedbackConsumer : public graphflow::core::GraphNode {
 public:
  FeedbackConsumer() : GraphNode("test.feedback", nullptr) {}

  std::vector<SenderFeedback> updates;

 protected:
  bool doInit() override {
    subscribe<nodes::SenderFeedbackEvent>(
        [this](const auto& event) { updates.push_back(event.feedback); });
    return true;
  }
};

void pipeline_contract() {
  auto h = std::make_shared<Harness>();
  RoomSession room(std::make_unique<FakeTransport>(h));
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
  check(feedback->updates.size() == 1 && feedback->updates.back().epoch == 1,
        "Initial feedback must precede producer activation");
  check(send_a.write(frame(1, 1)) == WriteStatus::accepted, "Publish edge did not deliver media");
  check(send_b.write(frame(1, 1)) == WriteStatus::accepted,
        "Second publish edge did not deliver media");
  send_a.poll_feedback();
  check(feedback->updates.size() == 2, "Feedback edge did not deliver feedback");
  BindingToken remote;
  {
    std::lock_guard lock(h->mutex);
    remote = h->subscribed.front();
  }
  TransportEvent e{TransportEventKind::frame, remote};
  e.frame = frame(1, 3000);
  h->push(std::move(e));
  eventually([&] { return receiver->snapshot().queued_frames == 1; }, "Receive packet not queued");
  check(consumer->frames.empty(), "Native callback bypassed graph scheduling");
  check(receive.poll() == 1 && consumer->frames.size() == 1,
        "Subscription graph did not route encoded frame");
  check(consumer->frames[0].timestamp == 3000, "Subscription graph modified media timestamp");
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

void engine_contract() {
  auto h = std::make_shared<Harness>();
  engine::RtcEngine engine(std::make_unique<FakeTransport>(h));
  get(engine.join(options()));
  auto first = engine.publish(video("first"));
  auto second = engine.publish(video("second"));
  auto consumer = std::make_shared<FrameConsumer>();
  auto incoming = engine.subscribe(video("incoming"), consumer);
  eventually(
      [&] {
        engine.poll();
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
  auto cancelled = engine.publish(video("cancel"));
  get(engine.unpublish("cancel"));
  bool rejected = false;
  try {
    get(std::move(cancelled));
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "Pending engine graph was not cancelled");
  engine.poll();
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
        engine.poll();
        return engine.write("second", frame(1, 2)) == WriteStatus::closed;
      },
      "Server revoke did not close publication graph");
  check(!engine.next_failure(), "Normal server removal became a graph processing failure");
  bool removal_observed = false;
  while (auto event = engine.room().next_event(0ms))
    if (event->kind == RoomEventKind::track_removed && event->track_id == "second")
      removal_observed = event->descriptor && event->descriptor->id == "second";
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
  engine::RtcEngine engine(std::make_unique<FakeTransport>(h));
  get(engine.join(options()));
  int activations = 0, quiescences = 0, polls = 0;
  bool teardown_while_running = false;
  std::shared_ptr<GraphProducer> producer;
  auto publication = engine.publish_graph(video("composed"), [&](auto sink) {
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
    spec.poll = [&] { ++polls; };
    return spec;
  });
  eventually(
      [&] {
        engine.poll();
        return publication.wait_for(0ms) == std::future_status::ready;
      },
      "Composed graph never started");
  check(publication.get() == "composed" && activations == 1 && polls > 0,
        "Composed graph lifecycle failed");
  eventually(
      [&] {
        std::lock_guard lock(h->mutex);
        return !h->sent.empty();
      },
      "Composed graph edges did not reach transport");
  auto consumer = std::make_shared<FrameConsumer>();
  auto subscription = engine.subscribe_graph(video("receive-composed"), [consumer](auto source) {
    auto relay = std::make_shared<GraphRelay>();
    engine::TrackGraphSpec spec;
    spec.nodes = {consumer, relay};
    spec.connections = {engine::TrackGraphSpec::connect<nodes::EncodedFrameEvent>(source, relay),
                        engine::TrackGraphSpec::connect<nodes::EncodedFrameEvent>(relay, consumer)};
    return spec;
  });
  eventually(
      [&] {
        engine.poll();
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
  eventually(
      [&] {
        engine.poll();
        return !consumer->frames.empty();
      },
      "Receive composition dropped media");
  get(engine.unpublish("composed"));
  check(quiescences == 1 && teardown_while_running, "Graph stop preceded producer quiescence");
  check(engine.room().snapshot().state == ConnectionState::connected,
        "Graph close left shared room");
  auto failing = engine.publish_graph(video("failure"), [&](auto) {
    engine::TrackGraphSpec spec;
    spec.activate = [] { throw std::runtime_error("source activation failed"); };
    spec.quiesce = [&] { ++quiescences; };
    return spec;
  });
  eventually(
      [&] {
        engine.poll();
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
void typed_join_failure() {
  auto h = std::make_shared<Harness>();
  h->connect = false;
  RoomSession room(std::make_unique<FakeTransport>(h));
  RoomOptions options;
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
#ifdef LIVEKIT_TEST_RTC_NODES
    pipeline_contract();
    engine_contract();
    composed_graph_contract();
    std::cout << "RTC graph pipeline contracts passed\n";
#else
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
