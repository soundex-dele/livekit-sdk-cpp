#include "livekit/rtc/room_engine.hpp"
#include "backends/rtc/room_transport.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#ifdef LIVEKIT_TEST_RTC_NODES
#include "livekit/rtc/graph_api.hpp"
#endif

using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::detail;
using namespace std::chrono_literals;

namespace {
void check(bool ok, const char* message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

struct Wire {
  RoomTransport::Events emit;
  std::map<std::pair<TrackId, bool>, BindingToken> bindings;
  std::vector<EncodedFrame> sent;
  std::vector<RoomTransport::Removed> removals;
  Epoch epoch = 0;
  int starts = 0, stops = 0, keyframes = 0;
  bool writable = true;
  std::vector<std::string> order;
};

class Transport final : public RoomTransport {
 public:
  explicit Transport(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}

  void start(const RoomOptions&, Epoch epoch) override {
    ++wire_->starts;
    wire_->epoch = epoch;
    wire_->emit = [this](auto events) { notify_events(std::move(events)); };
  }

  void publish(const BindingToken& binding, const TrackDescriptor&) override {
    wire_->bindings[{binding.id, true}] = binding;
  }

  void subscribe(const BindingToken& binding, const TrackDescriptor&,
                 std::shared_ptr<QueueBudget>) override {
    wire_->bindings[{binding.id, false}] = binding;
  }

  void remove(const BindingToken& binding, bool, Removed done) override {
    wire_->order.push_back("remove:" + binding.id);
    wire_->removals.push_back(std::move(done));
  }

  SendResult send(const BindingToken&, const EncodedFrame& frame) override {
    if (!wire_->writable) {
      return SendResult::would_block;
    }
    wire_->sent.push_back(frame);
    return SendResult::sent;
  }

  void request_keyframe(const BindingToken&) override {
    ++wire_->keyframes;
  }

  void stop() noexcept override {
    wire_->emit = {};
    ++wire_->stops;
    wire_->order.push_back("stop");
  }

 private:
  std::shared_ptr<Wire> wire_;
};

auto factory(const std::shared_ptr<Wire>& wire) {
  return [wire] { return std::make_unique<Transport>(wire); };
}

TrackDescriptor descriptor(std::string id) {
  TrackDescriptor track;
  track.id = std::move(id);
  track.format.width = 16;
  track.format.height = 16;
  return track;
}

void joined(const std::shared_ptr<Wire>& wire, std::vector<TransportEvent> initial = {}) {
  TransportEvent roster{TransportEventKind::session_changed};
  roster.session = std::make_shared<SessionInfo>();
  initial.push_back({TransportEventKind::connected});
  initial.push_back(roster);
  wire->emit(std::move(initial));
}

void ready(const std::shared_ptr<Wire>& wire, const TrackId& id, bool publication) {
  TransportEvent event{publication ? TransportEventKind::published
                                   : TransportEventKind::subscribed};
  event.binding = wire->bindings.at({id, publication});
  event.server_sid = "server-" + id;
  wire->emit({std::move(event)});
}

EncodedFrame frame(Epoch epoch) {
  EncodedFrame result;
  result.buffer = EncodedBuffer({0, 0, 0, 1, 0x65, 0x80});
  result.epoch = epoch;
  result.keyframe = true;
  return result;
}

struct Observer final : RoomObserver {
  int joins = 0, leaves = 0, unsubscribed = 0;
  std::thread::id thread;
  std::shared_ptr<PublishedTrack> publication;
  std::shared_ptr<SubscribedTrack> subscription;

  void on_joined(const RoomSnapshot& room) override {
    check(room.state == ConnectionState::connected && bool(room.session), "join snapshot/roster");
    ++joins;
    thread = std::this_thread::get_id();
  }

  void on_left() override {
    ++leaves;
  }

  void on_published(std::shared_ptr<PublishedTrack> track) override {
    publication = track;
  }

  void on_subscribed(std::shared_ptr<SubscribedTrack> track) override {
    subscription = track;
  }

  void on_unsubscribed(const TrackId&) override {
    ++unsubscribed;
  }
};

void room_contract() {
  auto wire = std::make_shared<Wire>();
  Observer observer;
  {
    RoomEngine room(factory(wire));
    room.set_observer(&observer);
    room.join({});
    check(wire->starts == 1 && observer.joins == 0, "join must only initiate work");
    std::thread::id notifier;
    std::thread producer([&] {
      notifier = std::this_thread::get_id();
      joined(wire);
    });
    producer.join();
    check(observer.joins == 1 && observer.thread == notifier,
          "Backend pushes observer notification without application servicing");
    room.publish(descriptor("video"));
    auto remote = descriptor("video");
    remote.queue.max_frames = 1;
    room.subscribe(remote);
    check(!observer.publication && !observer.subscription, "no premature track success");
    ready(wire, "video", true);
    ready(wire, "video", false);
    check(observer.publication && observer.subscription, "separate publication/subscription IDs");
    check(observer.publication->snapshot().state == TrackState::ready, "publication ready");
    check(observer.subscription->snapshot().state == TrackState::ready, "subscription ready");
    check(wire->keyframes == 1, "New video subscription must request its first keyframe");
    check(observer.subscription->request_keyframe() == ErrorCode::ok, "keyframe command");
    check(wire->keyframes == 2, "Explicit keyframe request reaches transport immediately");
    check(observer.publication->try_write(frame(wire->epoch)) == WriteStatus::accepted, "write");
    TransportEvent received{TransportEventKind::frame};
    received.binding = wire->bindings.at({"video", false});
    received.frame = frame(wire->epoch);
    wire->emit({received});
    check(wire->sent.size() == 1, "encoded publish transport");
    wire->writable = false;
    auto blocked = frame(wire->epoch);
    blocked.timestamp = 1;
    check(observer.publication->try_write(std::move(blocked)) == WriteStatus::accepted,
          "queue frame while transport blocked");
    check(wire->sent.size() == 1, "blocked transport retains frame");
    wire->writable = true;
    wire->emit({{TransportEventKind::writable, wire->bindings.at({"video", true})}});
    check(wire->sent.size() == 2, "backend writable callback resumes send without polling");
    check(observer.subscription->read(0ms).status == ReadStatus::frame,
          "encoded subscribe transport");
    received.frame = frame(wire->epoch);
    received.frame->timestamp = 1;
    wire->emit({received});
    received.frame->timestamp = 2;
    received.frame->keyframe = false;
    wire->emit({received});
    check(wire->keyframes == 3, "Receive overflow immediately requests a recovery keyframe");
    received.frame->timestamp = 3;
    wire->emit({received});
    check(wire->keyframes == 3, "Dependent frames do not duplicate recovery requests");
    room.unsubscribe("video");
    check(observer.unsubscribed == 0, "wait for native detachment");
    wire->removals.back()({});
    check(observer.unsubscribed == 1 && room.snapshot().publications == 1, "one-track teardown");
    room.leave();
    check(observer.leaves == 1 && wire->stops == 1, "leave notification");
    check(observer.publication->snapshot().state == TrackState::closed, "close endpoint on leave");
    room.join({});
    joined(wire);
    check(observer.joins == 2 && room.snapshot().epoch == 2, "explicit rejoin");
    room.set_observer(nullptr);
  }
  check(observer.leaves == 1 && wire->stops == 2, "detach observer before destruction");
}

void callbacks_are_optional_and_exceptions_propagate() {
  auto wire = std::make_shared<Wire>();
  RoomEngine room(factory(wire));
  room.join({});
  joined(wire);
  room.leave();

  struct Throwing final : RoomObserver {
    void on_joined(const RoomSnapshot&) override {
      throw std::runtime_error("caller callback");
    }
  } observer;

  room.set_observer(&observer);
  room.join({});
  bool propagated = false;
  try {
    joined(wire);
  } catch (const std::runtime_error& error) {
    propagated = std::string(error.what()) == "caller callback";
  }
  check(propagated, "RoomEngine must not catch or translate callback exceptions");
  room.set_observer(nullptr);
}
#ifdef LIVEKIT_TEST_RTC_NODES
class Consumer final : public graphflow::core::GraphNode {
 public:
  Consumer() : GraphNode("consumer", nullptr) {}

  std::atomic<int> frames{0};

 protected:
  bool doInit() override {
    subscribe<nodes::EncodedFrameEvent>([this](const auto&) { ++frames; });
    return true;
  }
};

void engine_contract() {
  using namespace engine;
  auto wire = std::make_shared<Wire>();
  auto consumer = std::make_shared<Consumer>();
  bool active = false;
  int joins = 0, subscriptions = 0, publications = 0, removed = 0, leaves = 0;
  RtcEngine rtc(factory(wire));
  RtcCallbacks callbacks;
  callbacks.on_joined = [&](const auto&) { ++joins; };
  callbacks.on_published = [&](const MediaPublication& value) {
    ++publications;
    check(value.id == "local", "publication callback identity");
    check(rtc.publication(value.id).state == TrackState::ready,
          "publication ready before callback");
  };
  callbacks.on_subscribed = [&](const MediaSubscription& value) {
    check(active && value.id == "remote", "graph activated before subscribed callback");
    check(consumer->frames == 0, "subscription success is independent of first frame");
    ++subscriptions;
  };
  callbacks.on_unsubscribed = [&](const TrackId&) { ++removed; };
  callbacks.on_left = [&] { ++leaves; };
  callbacks.on_track_published = [&](const TrackDescriptor& track) {
    check(joins == 1, "join callback precedes discovery even in reordered native batches");
    rtc.graph().subscribe_graph(track, [&](auto source) {
      TrackGraphSpec spec;
      spec.nodes.push_back(consumer);
      spec.connections.push_back(
          TrackGraphSpec::connect<nodes::EncodedFrameEvent>(source, consumer));
      spec.activate = [&] { active = true; };
      spec.quiesce = [&] {
        active = false;
        wire->order.push_back("quiesce:remote");
      };
      return spec;
    });
  };
  rtc.set_callbacks(std::move(callbacks));
  rtc.join({});
  check(joins == 0, "engine must not synthesize join success");
  TransportEvent available{TransportEventKind::remote_track_added};
  available.descriptor = descriptor("remote");
  joined(wire, {available});
  check(joins == 1 && subscriptions == 0, "observer join forwarding / discovery");
  rtc.publish(descriptor("local"));
  ready(wire, "local", true);
  ready(wire, "remote", false);
  check(publications == 1 && subscriptions == 1, "track callbacks");
  check(rtc.write("local", frame(wire->epoch)) == WriteStatus::accepted, "GraphEngine publication");
  TransportEvent received{TransportEventKind::frame};
  received.binding = wire->bindings.at({"remote", false});
  received.frame = frame(wire->epoch);
  wire->emit({received});
  const auto until = SteadyClock::now() + 2s;
  while (consumer->frames == 0 && SteadyClock::now() < until) {
    std::this_thread::sleep_for(1ms);
  }
  check(consumer->frames == 1 && wire->sent.size() == 1, "real GraphEngine encoded frame delivery");
  rtc.unsubscribe("remote");
  check(!active && wire->order[0] == "quiesce:remote" && wire->order[1] == "remove:remote",
        "stop graph before detaching endpoint");
  check(removed == 0, "no premature unsubscribe callback");
  wire->removals.back()({});
  check(removed == 1 && rtc.snapshot().state == ConnectionState::connected,
        "unsubscribe preserves room");
  rtc.leave();
  check(leaves == 1 && wire->stops == 1, "engine leave callback");
}
#endif
}  // namespace

int main() {
  room_contract();
  callbacks_are_optional_and_exceptions_propagate();
#ifdef LIVEKIT_TEST_RTC_NODES
  engine_contract();
#endif
  std::cout << "RTC observer contracts passed\n";
}
