#include "backends/rtc/room_engine.hpp"
#include <iostream>
#include <thread>

using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::detail;
using namespace std::chrono_literals;

namespace {
void check(bool value, const char* message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

struct Wire {
  std::vector<TransportEvent> events;
  std::optional<ParticipantUpdateRequest> update;
  int starts = 0;
};

class Transport final : public RoomTransport {
 public:
  explicit Transport(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}

  BackendCapabilities capabilities() const override {
    BackendCapabilities value;
    value.participant_update_ack = true;
    return value;
  }

  void start(const RoomOptions&, Epoch) override {
    ++wire_->starts;
    wire_->events.push_back({TransportEventKind::connected});
    wakeup_->signal();
  }

  std::vector<TransportEvent> take_events() override {
    return std::exchange(wire_->events, {});
  }

  void publish(const BindingToken&, const TrackDescriptor&) override {}

  void subscribe(const BindingToken&, const TrackDescriptor&,
                 std::shared_ptr<QueueBudget>) override {}

  void remove(const BindingToken&, bool, Removed done) override {
    if (done) {
      done({});
    }
  }

  SendResult send(const BindingToken&, const EncodedFrame&) override {
    return SendResult::sent;
  }

  void request_keyframe(const BindingToken&) override {}

  void update_participant(ParticipantUpdateRequest request) override {
    wire_->update = std::move(request);
  }

  void stop() noexcept override {}

 private:
  std::shared_ptr<Wire> wire_;
};

struct Fixture {
  std::shared_ptr<Wire> wire = std::make_shared<Wire>();
  std::vector<RoomEvent> events;
  std::unique_ptr<RoomEngine> room;

  Fixture() {
    RoomEngine::Hooks hooks;
    hooks.wakeup = std::make_shared<livekit::sdk::detail::WakeEvent>();
    hooks.event = [this](RoomEvent event, bool, bool) {
      events.push_back(std::move(event));
      return true;
    };
    hooks.reset_events = [this](size_t) { events.clear(); };
    hooks.attach_services = [](RoomTransport&) {};
    hooks.removal = [](auto) -> RoomTransport::Removed { return [](std::exception_ptr) {}; };
    room = std::make_unique<RoomEngine>([wire = wire] { return std::make_unique<Transport>(wire); },
                                        std::move(hooks));
  }

  void connect() {
    bool completed = false;
    room->join({}, [&](Result<void> result) { completed = bool(result); });
    room->poll();
    check(completed, "Join did not finish after owner polled transport");
  }
};

void owner_driven_and_no_thread_affinity() {
  Fixture f;
  int completions = 0;
  std::thread::id callback_thread;
  const auto caller = std::this_thread::get_id();
  std::thread first([&] {
    f.room->join({}, [&](Result<void> result) {
      if (result) {
        ++completions;
      }
      callback_thread = std::this_thread::get_id();
    });
  });
  first.join();
  check(f.wire->starts == 1 && completions == 0, "Join waited for or manufactured a server result");
  std::this_thread::sleep_for(10ms);
  check(completions == 0 && f.events.empty(),
        "Protocol component processed events without its owner");
  f.room->poll();
  check(completions == 1 && callback_thread == caller,
        "Protocol component imposed thread affinity");
  f.room->poll();
  check(completions == 1, "Operation completion was delivered twice");
  check(f.room->snapshot().state == ConnectionState::connected, "Snapshot did not advance");
}

void ack_and_interruption_are_distinct_from_submission() {
  Fixture f;
  f.connect();
  int completions = 0;
  ErrorCode code = ErrorCode::ok;
  f.room->update_local_participant({}, {}, [&](Result<void> result) {
    ++completions;
    code = result ? ErrorCode::ok : result.error().code;
  });
  check(f.wire->update.has_value() && completions == 0, "Participant update completed before ACK");
  TransportEvent ack{TransportEventKind::control_completed};
  ack.operation_id = f.wire->update->id;
  ack.binding.epoch = f.room->snapshot().epoch;
  f.wire->events.push_back(ack);
  f.room->poll();
  check(completions == 1 && code == ErrorCode::ok, "ACK did not settle operation");
  f.wire->events.push_back(ack);
  f.room->poll();
  check(completions == 1, "Duplicate ACK settled operation twice");

  f.room->update_local_participant({}, {}, [&](Result<void> result) {
    ++completions;
    code = result ? ErrorCode::ok : result.error().code;
  });
  ack.operation_id = f.wire->update->id;
  f.room->poll(true);
  check(completions == 2 && code == ErrorCode::outcome_unknown,
        "Cancellation lost unknown remote outcome");
  f.wire->events.push_back(ack);
  f.room->poll();
  check(completions == 2, "Late ACK revived a cancelled operation");
}

void immediate_failure_and_owner_driven_timeout() {
  Fixture f;
  int completed = 0;
  ErrorCode code = ErrorCode::ok;
  RoomOptions invalid;
  invalid.max_tracks = 0;
  f.room->join(invalid, [&](Result<void> result) {
    ++completed;
    code = result ? ErrorCode::ok : result.error().code;
  });
  check(completed == 1 && code == ErrorCode::invalid_argument && f.wire->starts == 0,
        "Local validation did not report its own result");

  RoomOptions options;
  options.operation_timeout = 1ms;
  f.room->join(options, [&](Result<void> result) {
    ++completed;
    code = result ? ErrorCode::ok : result.error().code;
  });
  f.wire->events.clear();
  check(f.room->next_deadline().has_value(), "Owner cannot schedule protocol timeout");
  std::this_thread::sleep_for(15ms);
  check(completed == 1, "Protocol component ran its own timer");
  f.room->poll();
  check(completed == 2 && code == ErrorCode::timeout, "Owner servicing did not settle timeout");
}
}  // namespace

int main() {
  try {
    owner_driven_and_no_thread_affinity();
    ack_and_interruption_are_distinct_from_submission();
    immediate_failure_and_owner_driven_timeout();
    std::cout << "Room state machine contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
