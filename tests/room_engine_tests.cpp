#include "livekit/rtc/room_engine.hpp"
#include "backends/rtc/room_transport.hpp"
#include "backends/rtc/execution.hpp"
#include "rtc_blocking_bridge.hpp"
#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#ifdef LIVEKIT_TEST_RTC_NODES
#include "livekit/rtc/graph_api.hpp"
#endif
using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::detail;
using namespace livekit::sdk::app_support;
using namespace std::chrono_literals;

namespace {
void check(bool ok, const char* message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

template <class F>
void eventually(F fn) {
  const auto until = SteadyClock::now() + 5s;
  while (!fn()) {
    check(SteadyClock::now() < until, "Contract timed out");
    std::this_thread::sleep_for(1ms);
  }
}

struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, open = false;

  void block() {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    check(changed.wait_for(lock, 5s, [&] { return open; }), "Gate timed out");
  }

  void wait() {
    std::unique_lock lock(mutex);
    check(changed.wait_for(lock, 5s, [&] { return entered; }), "Gate entry timed out");
  }

  void release() {
    std::lock_guard lock(mutex);
    open = true;
    changed.notify_all();
  }
};

struct Wire {
  std::mutex mutex;
  RoomTransport::Events emit;
  std::map<std::pair<TrackId, bool>, BindingToken> bindings;
  std::vector<RoomTransport::Removed> removals;
  std::vector<EncodedFrame> sent;
  std::atomic<int> starts{0}, stops{0}, keyframes{0}, mutes{0};
  std::atomic<bool> writable{true}, delay_remove{false}, auto_ready{true}, auto_join{true};
  std::thread::id control_thread;
  std::shared_ptr<Gate> start_gate, mute_gate, stop_gate;

  void events(std::vector<TransportEvent> values) {
    std::lock_guard lock(mutex);
    if (emit) {
      emit(std::move(values));
    }
  }

  BindingToken binding(const TrackId& id, bool pub = true) {
    eventually([&] {
      std::lock_guard lock(mutex);
      return bindings.contains({id, pub});
    });
    std::lock_guard lock(mutex);
    return bindings.at({id, pub});
  }

  size_t sent_count() {
    std::lock_guard lock(mutex);
    return sent.size();
  }

  size_t removing() {
    std::lock_guard lock(mutex);
    return removals.size();
  }

  void detached(Failure failure = {}) {
    RoomTransport::Removed done;
    {
      std::lock_guard lock(mutex);
      done = std::move(removals.front());
      removals.erase(removals.begin());
    }
    done(std::move(failure));
  }
};

class Transport final : public RoomTransport {
 public:
  explicit Transport(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}

  Result<void> start(const RoomOptions&, Epoch) override {
    if (wire_->start_gate) {
      wire_->start_gate->block();
    }
    wire_->control_thread = std::this_thread::get_id();
    ++wire_->starts;
    {
      std::lock_guard lock(wire_->mutex);
      wire_->emit = [this](auto events) { notify_events(std::move(events)); };
    }
    TransportEvent roster{TransportEventKind::session_changed};
    roster.session = std::make_shared<SessionInfo>();
    if (wire_->auto_join) {
      notify_events({{TransportEventKind::connected}, roster});
    }
    return Result<void>::success();
  }

  Result<void> publish(const BindingToken& b, const TrackDescriptor&) override {
    ready(b);
    return Result<void>::success();
  }

  Result<void> subscribe(const BindingToken& b, const TrackDescriptor&,
                         std::shared_ptr<QueueBudget>) override {
    ready(b);
    return Result<void>::success();
  }

  Result<void> remove(const BindingToken&, bool, Removed done) override {
    if (wire_->delay_remove) {
      std::lock_guard lock(wire_->mutex);
      wire_->removals.push_back(std::move(done));
    } else if (done) {
      done({});
    }
    return Result<void>::success();
  }

  SendResult send(const BindingToken&, const EncodedFrame& frame) override {
    if (!wire_->writable) {
      return SendResult::would_block;
    }
    std::lock_guard lock(wire_->mutex);
    wire_->sent.push_back(frame);
    return SendResult::sent;
  }

  void request_keyframe(const BindingToken&) override {
    ++wire_->keyframes;
  }

  Result<void> set_muted(MuteRequest request) override {
    if (wire_->mute_gate) {
      wire_->mute_gate->block();
    }
    ++wire_->mutes;
    TransportEvent event{TransportEventKind::control_completed, request.binding};
    event.operation_id = request.id;
    notify_events({event});
    return Result<void>::success();
  }

  void stop() noexcept override {
    if (wire_->stop_gate) {
      wire_->stop_gate->block();
    }
    std::lock_guard lock(wire_->mutex);
    wire_->emit = {};
    ++wire_->stops;
  }

 private:
  void ready(const BindingToken& b) {
    {
      std::lock_guard lock(wire_->mutex);
      wire_->bindings[{b.id, b.publication}] = b;
    }
    if (wire_->auto_ready) {
      notify_events(
          {{b.publication ? TransportEventKind::published : TransportEventKind::subscribed, b,
            "sid"}});
    }
  }

  std::shared_ptr<Wire> wire_;
};

auto factory(std::shared_ptr<Wire> wire) {
  return [wire] { return std::make_unique<Transport>(wire); };
}

TrackDescriptor descriptor(std::string id) {
  TrackDescriptor d;
  d.id = std::move(id);
  d.format.width = 16;
  d.format.height = 16;
  return d;
}

EncodedFrame frame(Epoch epoch, uint64_t timestamp = 0) {
  EncodedFrame f;
  f.buffer = EncodedBuffer({0, 0, 0, 1, 0x65, 0x80});
  f.epoch = epoch;
  f.keyframe = true;
  f.timestamp = timestamp;
  return f;
}

void room_contract() {
  auto wire = std::make_shared<Wire>();
  BlockingRoom room(factory(wire));
  require_result(room.join({}));
  check(room.snapshot().session != nullptr, "Join roster visible");
  auto pub = require_result(room.publish(descriptor("same")));
  auto sub = require_result(room.subscribe(descriptor("same")));
  eventually([&] { return wire->keyframes == 1; });
  check(pub->try_write(frame(pub->snapshot().epoch)) == WriteStatus::accepted, "Write accepted");
  eventually([&] { return wire->sent_count() == 1; });
  wire->writable = false;
  check(pub->try_write(frame(pub->snapshot().epoch, 1)) == WriteStatus::accepted,
        "Blocked write queued");
  eventually([&] { return pub->snapshot().queued_frames == 1; });
  wire->writable = true;
  wire->events({{TransportEventKind::writable, wire->binding("same")}});
  eventually([&] { return wire->sent_count() == 2; });
  TransportEvent received{TransportEventKind::frame, wire->binding("same", false)};
  received.frame = frame(sub->snapshot().epoch);
  wire->events({received});
  check(sub->read(1s).status == ReadStatus::frame, "Receive through control worker");
  require_result(room.unsubscribe("same"));
  check(room.snapshot().publications == 1 && room.snapshot().state == ConnectionState::connected,
        "Single-track close preserves room");
  require_result(room.leave());
  require_result(room.join({}));
  check(room.snapshot().epoch > pub->snapshot().epoch, "Explicit rejoin");
  require_result(room.leave());
}

// Single-call probes retain results even when a callback arrives before return.
template <class T>
struct Probe {
  std::promise<Result<T>> result;
  std::atomic<int> calls{0};
  std::thread::id thread;

  Completion<T> callback() {
    return [this](Result<T> value) {
      thread = std::this_thread::get_id();
      if (calls.fetch_add(1) == 0) {
        result.set_value(std::move(value));
      }
    };
  }

  Result<T> get() {
    auto future = result.get_future();
    check(future.wait_for(5s) == std::future_status::ready, "Missing completion");
    return future.get();
  }
};

void completion_contract() {
  auto wire = std::make_shared<Wire>();
  wire->auto_join = false;
  RoomEngine room(factory(wire));
  Probe<RoomSnapshot> joined;
  room.join({}, joined.callback());
  check(joined.calls == 0, "Join must wait for connected");
  // Replacing the continuous observer must not erase an operation callback.
  RoomObserver observer;
  require_result(room.set_observer(&observer));
  require_result(room.set_observer(nullptr));
  wire->events({{TransportEventKind::connected}});
  check(bool(joined.get()), "Join completion without observer");
  check(joined.thread != wire->control_thread && joined.thread != std::this_thread::get_id(),
        "Completion runs on notification");
  wire->events({{TransportEventKind::connected}, {TransportEventKind::connected}});
  (void)room.snapshot();
  require_result(room.set_observer(nullptr));
  check(joined.calls == 1, "Reconnect/duplicate event must not replay join completion");

  Probe<std::shared_ptr<PublishedTrack>> ready;
  room.publish(descriptor("complete"), ready.callback());
  auto publication = ready.get();
  check(bool(publication), "Published endpoint result");
  Probe<std::shared_ptr<PublishedTrack>> duplicate;
  room.publish(descriptor("complete"), duplicate.callback());
  check(duplicate.get().error().code == ErrorCode::busy, "Local rejection is a Result");
  Probe<std::shared_ptr<SubscribedTrack>> unknown;
  room.subscribe(TrackId("missing"), unknown.callback());
  check(unknown.get().error().code == ErrorCode::invalid_argument, "Unknown remote track result");
  Probe<void> muted;
  room.set_muted("complete", true, muted.callback());
  check(bool(muted.get()), "Control result uses operation id");
  Probe<void> unsupported;
  room.set_subscription_permissions({}, unsupported.callback());
  check(unsupported.get().error().code == ErrorCode::unsupported,
        "Unsupported transport control reports an explicit result");

  wire->auto_ready = false;
  Probe<std::shared_ptr<PublishedTrack>> failed;
  room.publish(descriptor("failed"), failed.callback());
  auto failure = TransportEvent{TransportEventKind::operation_failed, wire->binding("failed")};
  failure.error = "Backend rejected publication";
  wire->events({failure, failure});
  check(failed.get().error().code == ErrorCode::request_failed, "Backend failure reaches caller");

  Probe<std::shared_ptr<SubscribedTrack>> cancelled;
  room.subscribe(descriptor("pending"), cancelled.callback());
  wire->delay_remove = true;
  Probe<void> removed, also_removed;
  room.unpublish("complete", removed.callback());
  room.unpublish("complete", also_removed.callback());
  check(removed.calls == 0 && also_removed.calls == 0, "Removal waits for native detach");
  (void)room.snapshot();  // Both remove requests have reached control.
  wire->detached();
  check(bool(removed.get()) && bool(also_removed.get()), "Each remove invocation has a result");
  Probe<void> left;
  room.leave(left.callback());
  check(bool(left.get()), "Synchronous leave delivers its result");
  check(cancelled.get().error().code == ErrorCode::cancelled, "Leave settles pending subscription");
  check(failed.calls == 1 && removed.calls == 1 && joined.calls == 1, "Results delivered once");

  Probe<RoomSnapshot> join_failed;
  room.join({}, join_failed.callback());
  (void)room.snapshot();  // Install the new transport before emitting its event.
  TransportEvent terminal{TransportEventKind::terminal_error};
  terminal.error = "Join denied";
  wire->events({terminal});
  check(join_failed.get().error().code == ErrorCode::connection_lost, "Terminal join failure");
  room.leave();
  Probe<RoomSnapshot> join_cancelled;
  room.join({}, join_cancelled.callback());
  room.leave();
  check(join_cancelled.get().error().code == ErrorCode::cancelled, "Leave cancels unfinished join");
}

void posted_room_contract() {
  auto wire = std::make_shared<Wire>();
  wire->start_gate = std::make_shared<Gate>();
  RoomEngine room(factory(wire));
  Probe<RoomSnapshot> joined;
  auto submitted = std::async(std::launch::async, [&] { room.join({}, joined.callback()); });
  wire->start_gate->wait();
  const bool returned = submitted.wait_for(100ms) == std::future_status::ready;
  wire->start_gate->release();
  submitted.get();
  check(returned, "Join returns while transport start is busy");
  check(bool(joined.get()), "Posted join completes");

  Probe<std::shared_ptr<PublishedTrack>> published;
  room.publish(descriptor("control"), published.callback());
  check(bool(published.get()), "Control endpoint ready");
  TransportEvent remote{TransportEventKind::remote_track_added};
  remote.descriptor = descriptor("remote");
  wire->events({remote});
  wire->mute_gate = std::make_shared<Gate>();
  Probe<void> muted;
  room.set_muted("control", true, muted.callback());
  wire->mute_gate->wait();
  Probe<std::shared_ptr<SubscribedTrack>> subscribed;
  Probe<void> removed;
  auto commands = std::async(std::launch::async, [&] {
    room.subscribe(TrackId("remote"), subscribed.callback());
    room.unsubscribe("remote", removed.callback());
  });
  const bool posted = commands.wait_for(100ms) == std::future_status::ready;
  wire->mute_gate->release();
  commands.get();
  check(posted, "Requests return while control is busy");
  check(bool(muted.get()), "Posted control completes");
  auto subscription = subscribed.get();
  check(bool(subscription), "Remote ID subscription completes");
  check(bool(removed.get()) && room.snapshot().subscriptions == 0,
        "Remote ID resolution cannot move subscribe behind unsubscribe");
  room.leave();
}

template <class Engine>
void closing_admission_contract() {
  auto wire = std::make_shared<Wire>();
  Engine engine(factory(wire));
  Probe<RoomSnapshot> joined;
  engine.join({}, joined.callback());
  check(bool(joined.get()), "Admission room joined");
  wire->stop_gate = std::make_shared<Gate>();
  Probe<RoomSnapshot> callback_join;
  auto leaving = std::async(std::launch::async, [&] {
    engine.leave([&](Result<void> result) {
      check(bool(result), "Leave completion succeeds");
      engine.join({}, callback_join.callback());
    });
  });
  wire->stop_gate->wait();
  Probe<RoomSnapshot> rejected;
  engine.join({}, rejected.callback());
  auto result = rejected.get();
  wire->stop_gate->release();
  check(leaving.wait_for(5s) == std::future_status::ready, "Leave finishes after stop");
  leaving.get();
  check(callback_join.get().error().code == ErrorCode::session_closed,
        "Leave completion cannot reopen the engine before its barrier returns");
  check(!result && result.error().code == ErrorCode::session_closed,
        "Join during leave is rejected asynchronously");
  check(wire->starts == 1 && engine.snapshot().state == ConnectionState::disconnected,
        "Queued requests cannot reopen a closing engine");
  Probe<RoomSnapshot> rejoined;
  engine.join({}, rejoined.callback());
  check(bool(rejoined.get()), "Admission reopens after leave returns");
  engine.leave();
}

void observer_thread_contract() {
  auto wire = std::make_shared<Wire>();
  std::promise<std::thread::id> joined;

  struct Observer final : RoomObserver {
    std::promise<std::thread::id>* result;

    void on_room_event(const RoomEvent& event) override {
      if (event.kind == RoomEventKind::connected) {
        result->set_value(std::this_thread::get_id());
      }
    }
  } observer;

  observer.result = &joined;
  RoomEngine room(factory(wire));
  require_result(room.set_observer(&observer));
  room.join({});
  auto result = joined.get_future();
  check(result.wait_for(5s) == std::future_status::ready, "Join callback delivered");
  const auto notification = result.get();
  check(notification != wire->control_thread && notification != std::this_thread::get_id(),
        "Standalone observer has its own notification worker");
  room.leave();
  require_result(room.set_observer(nullptr));
}

void observer_replacement_contract() {
  auto wire = std::make_shared<Wire>();
  RoomEngine room(factory(wire));
  Probe<RoomSnapshot> joined;
  room.join({}, joined.callback());
  check(bool(joined.get()), "Observer fence room joins");

  struct BlockingObserver final : RoomObserver {
    Gate gate;

    void on_room_event(const RoomEvent& event) override {
      if (event.kind == RoomEventKind::statistics) {
        gate.block();
      }
    }
  } blocking;

  require_result(room.set_observer(&blocking));
  wire->events({{TransportEventKind::statistics}});
  blocking.gate.wait();
  auto removed = std::async(std::launch::async, [&] { return room.set_observer(nullptr); });
  check(removed.wait_for(20ms) == std::future_status::timeout,
        "Observer replacement fences active callback");
  blocking.gate.release();
  check(removed.wait_for(5s) == std::future_status::ready, "Observer fence completes");
  require_result(removed.get());
  room.leave();
}

#ifdef LIVEKIT_TEST_RTC_NODES
void engine_completion_contract() {
  using namespace engine;
  auto wire = std::make_shared<Wire>();
  RtcEngine rtc(factory(wire));
  Probe<RoomSnapshot> joined;
  rtc.join({}, joined.callback());
  check(bool(joined.get()), "Engine join result");
  Probe<MediaPublication> failed;
  rtc.graph().publish_graph(descriptor("bad-graph"), {}, failed.callback());
  check(failed.get().error().code == ErrorCode::invalid_argument,
        "Build failure reaches completion");
  check(rtc.snapshot().publications == 0, "Failed graph automatically rolls back its endpoint");
  Probe<MediaPublication> retried;
  rtc.publish(descriptor("bad-graph"), retried.callback());
  check(bool(retried.get()), "Rolled-back track id can be reused");
  Probe<MediaSubscription> failed_subscription;
  rtc.graph().subscribe_graph(descriptor("bad-subscription"), {}, failed_subscription.callback());
  check(failed_subscription.get().error().code == ErrorCode::invalid_argument,
        "Subscription graph failure reaches completion");
  check(rtc.snapshot().subscriptions == 0, "Failed subscription graph rolls back its endpoint");
  Probe<MediaPublication> ready;
  rtc.publish(descriptor("healthy"), ready.callback());
  check(bool(ready.get()), "Engine still works after failure");
  Probe<void> wrong_thread;
  Probe<void> muted;
  rtc.set_muted("healthy", true, [&](Result<void> result) {
    // Ordinary requests can be chained from completion; synchronous leave cannot.
    rtc.leave(wrong_thread.callback());
    muted.callback()(std::move(result));
  });
  check(bool(muted.get()), "Control completion supports request chaining");
  check(wrong_thread.get().error().code == ErrorCode::invalid_thread,
        "Leave reports invalid callback thread");
  check(rtc.snapshot().state == ConnectionState::connected, "Rejected leave preserves room");
  wire->auto_ready = false;
  Probe<MediaSubscription> pending;
  rtc.subscribe(descriptor("pending"), pending.callback());
  rtc.leave();
  check(pending.get().error().code == ErrorCode::cancelled, "Engine forwards cancellation");
}

void rollback_detach_contract() {
  using namespace engine;
  auto wire = std::make_shared<Wire>();
  RtcEngine media(factory(wire));
  Probe<RoomSnapshot> joined;
  media.join({}, joined.callback());
  check(bool(joined.get()), "Rollback room joined");
  wire->delay_remove = true;
  Probe<MediaPublication> failed;
  media.graph().publish_graph(descriptor("rollback"), {}, failed.callback());
  eventually([&] { return wire->removing() == 1; });
  check(failed.calls == 0, "Graph failure waits for rollback before final completion");
  wire->detached();
  auto result = failed.get();
  check(!result && result.error().code == ErrorCode::invalid_argument,
        "Rollback preserves original graph failure");
  check(media.snapshot().publications == 0 && wire->stops == 0,
        "Rollback releases only the failed endpoint");
  media.close();
}

void rollback_failure_contract() {
  using namespace engine;
  auto wire = std::make_shared<Wire>();
  RtcEngine media(factory(wire));
  Probe<RoomSnapshot> joined;
  media.join({}, joined.callback());
  check(bool(joined.get()), "Rollback failure room joined");
  wire->delay_remove = true;
  Probe<MediaPublication> failed;
  media.graph().publish_graph(descriptor("rollback-failure"), {}, failed.callback());
  eventually([&] { return wire->removing() == 1; });
  wire->detached(
      make_failure(ErrorDomain::transport, ErrorCode::request_failed, "detach rejected"));
  auto result = failed.get();
  check(!result && result.error().code == ErrorCode::invalid_argument &&
            result.error().message.find("detach rejected") != std::string::npos,
        "Graph error retains rollback failure details");
  check(media.snapshot().publications == 1, "Failed detachment stays owned for cleanup retry");
  wire->delay_remove = false;
  Probe<void> removed;
  media.unpublish("rollback-failure", removed.callback());
  check(bool(removed.get()) && media.snapshot().publications == 0,
        "Unpublish retries failed native rollback");
  media.close();
  check(wire->stops == 1, "Closing media stops its owned room");
}

void three_thread_contract() {
  using namespace engine;
  auto wire = std::make_shared<Wire>();
  RtcEngine rtc(factory(wire));
  std::promise<void> joined, published;
  std::promise<std::thread::id> started, stopped;
  std::thread::id notification;
  auto callback_gate = std::make_shared<Gate>();

  struct Observer : RtcObserver {
    std::shared_ptr<Gate> gate;

    void on_room_event(const RoomEvent& event) override {
      if (event.kind == RoomEventKind::statistics) {
        gate->block();
      }
    }
  } observer;

  observer.gate = callback_gate;
  require_result(rtc.set_observer(&observer));
  rtc.join({}, [&](Result<RoomSnapshot> result) {
    check(bool(result), "Join result");
    joined.set_value();
  });
  auto join = joined.get_future();
  check(join.wait_for(5s) == std::future_status::ready, "Engine joined");
  auto graph_gate = std::make_shared<Gate>();
  rtc.graph().publish_graph(
      descriptor("video"),
      [&](auto) {
        started.set_value(std::this_thread::get_id());
        graph_gate->block();
        TrackGraphSpec graph;
        graph.quiesce = [&] { stopped.set_value(std::this_thread::get_id()); };
        return graph;
      },
      [&](Result<MediaPublication> result) {
        check(bool(result), "Graph publication result");
        notification = std::this_thread::get_id();
        published.set_value();
      });
  graph_gate->wait();
  Probe<MediaPublication> queued;
  auto submitted =
      std::async(std::launch::async, [&] { rtc.publish(descriptor("queued"), queued.callback()); });
  const bool returned = submitted.wait_for(100ms) == std::future_status::ready;
  if (!returned) {
    graph_gate->release();
  }
  submitted.get();
  check(returned, "Media request returns while lifecycle is busy");
  rtc.set_muted("video", true);
  eventually([&] { return wire->mutes == 1; });
  check(rtc.snapshot().state == ConnectionState::connected,
        "Slow graph startup does not block control");
  graph_gate->release();
  auto ready = published.get_future();
  check(ready.wait_for(5s) == std::future_status::ready, "Graph ready callback");
  check(bool(queued.get()), "Queued media request completes");
  auto lifecycle = started.get_future().get();
  check(lifecycle != wire->control_thread && lifecycle != notification &&
            notification != wire->control_thread,
        "Control, lifecycle and notification use three different threads");
  Probe<void> unmuted;
  rtc.set_muted("video", false, unmuted.callback());
  check(bool(unmuted.get()), "Unmute completes before writing");
  check(rtc.write("video", frame(rtc.snapshot().epoch)) == WriteStatus::accepted, "Encoded write");
  eventually([&] { return wire->sent_count() == 1; });
  wire->events({{TransportEventKind::statistics}});
  callback_gate->wait();
  rtc.set_muted("video", true);
  eventually([&] { return wire->mutes == 3; });
  auto leave = std::async(std::launch::async, [&] { rtc.leave(); });
  auto stop = stopped.get_future();
  check(stop.wait_for(5s) == std::future_status::ready, "Graph stopped before callback completes");
  check(stop.get() == lifecycle, "Graph startup and stop share lifecycle worker");
  check(leave.wait_for(20ms) == std::future_status::timeout, "Leave drains notification worker");
  callback_gate->release();
  check(leave.wait_for(5s) == std::future_status::ready, "Leave completes after callback");
  leave.get();
  check(wire->stops == 1, "Transport stopped once");
  require_result(rtc.set_observer(nullptr));
}
#endif
}  // namespace

int main() {
  try {
    room_contract();
    observer_thread_contract();
    completion_contract();
    observer_replacement_contract();
    posted_room_contract();
    closing_admission_contract<RoomEngine>();
#ifdef LIVEKIT_TEST_RTC_NODES
    three_thread_contract();
    closing_admission_contract<engine::RtcEngine>();
    engine_completion_contract();
    rollback_detach_contract();
    rollback_failure_contract();
#endif
    std::cout << "RTC room/media ownership contracts passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
