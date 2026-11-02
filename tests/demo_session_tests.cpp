#include "server_result.hpp"
#include "session.hpp"
#include "backends/rtc/room_transport.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>

using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::detail;
using namespace std::chrono_literals;

namespace {
void check(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

template <class F>
void until(F predicate, const char* message, std::chrono::seconds timeout = 10s) {
  const auto deadline = SteadyClock::now() + timeout;
  while (!predicate()) {
    check(SteadyClock::now() < deadline, message);
    std::this_thread::sleep_for(2ms);
  }
}

struct Wire {
  std::mutex mutex;
  RoomTransport::Events emit;
  std::map<TrackId, BindingToken> bindings;
  SessionInfo session;
  std::atomic<int> starts{0}, stops{0}, publishes{0}, subscribes{0}, removes{0};
  std::atomic<bool> auto_join{true}, hold_publish{false}, hold_subscribe{false};
  std::atomic<bool> reject_publish{false};

  void push(std::vector<TransportEvent> events) {
    std::lock_guard lock(mutex);
    if (emit) {
      emit(std::move(events));
    }
  }

  BindingToken binding(const TrackId& id) {
    std::lock_guard lock(mutex);
    const auto found = bindings.find(id);
    check(found != bindings.end(), "Expected pending transport binding");
    return found->second;
  }
};

class Transport final : public RoomTransport {
 public:
  explicit Transport(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}

  Result<void> start(const RoomOptions&, Epoch) override {
    {
      std::lock_guard lock(wire_->mutex);
      wire_->emit = [this](auto events) { notify_events(std::move(events)); };
      wire_->session.room.name = "async-demo";
      wire_->session.local.sid = "PA_local";
    }
    ++wire_->starts;
    if (wire_->auto_join) {
      wire_->push({{TransportEventKind::connected}, roster()});
    }
    return Result<void>::success();
  }

  Result<void> publish(const BindingToken& binding, const TrackDescriptor& descriptor) override {
    ++wire_->publishes;
    if (wire_->reject_publish) {
      return Result<void>::failure(make_failure(ErrorDomain::transport, ErrorCode::request_failed,
                                                "publication rejected: secret-token"));
    }
    {
      std::lock_guard lock(wire_->mutex);
      wire_->bindings[descriptor.id] = binding;
      PublicationInfo info;
      info.sid = "TR_" + descriptor.id;
      info.kind =
          descriptor.kind == MediaKind::video ? PublicationKind::video : PublicationKind::audio;
      wire_->session.local.publications[info.sid] = info;
    }
    if (!wire_->hold_publish) {
      wire_->push({{TransportEventKind::published, binding, "TR_" + descriptor.id}, roster()});
    }
    return Result<void>::success();
  }

  Result<void> subscribe(const BindingToken& binding, const TrackDescriptor& descriptor,
                         std::shared_ptr<QueueBudget>) override {
    {
      std::lock_guard lock(wire_->mutex);
      wire_->bindings[descriptor.id] = binding;
    }
    ++wire_->subscribes;
    if (!wire_->hold_subscribe) {
      wire_->push({{TransportEventKind::subscribed, binding, descriptor.remote_sid}});
    }
    return Result<void>::success();
  }

  Result<void> remove(const BindingToken& binding, bool, Removed done) override {
    ++wire_->removes;
    {
      std::lock_guard lock(wire_->mutex);
      wire_->bindings.erase(binding.id);
      wire_->session.local.publications.erase("TR_" + binding.id);
    }
    wire_->push({roster()});
    done({});
    return Result<void>::success();
  }

  SendResult send(const BindingToken&, const EncodedFrame&) override {
    return SendResult::sent;
  }

  void request_keyframe(const BindingToken&) override {}

  void stop() noexcept override {
    std::lock_guard lock(wire_->mutex);
    wire_->emit = {};
    ++wire_->stops;
  }

 private:
  TransportEvent roster() {
    TransportEvent event{TransportEventKind::session_changed};
    std::lock_guard lock(wire_->mutex);
    event.session = std::make_shared<SessionInfo>(wire_->session);
    return event;
  }

  std::shared_ptr<Wire> wire_;
};

demo::Session::EngineFactory factory(std::shared_ptr<Wire> wire, bool media = true) {
  return [wire, media] {
    auto engine =
        std::make_unique<engine::RtcEngine>([wire] { return std::make_unique<Transport>(wire); });
    if (media) {
      check(engine->enable_media() == ErrorCode::ok, "Media setup");
    }
    return Result<std::unique_ptr<engine::RtcEngine>>::success(std::move(engine));
  };
}

demo::SessionOptions options(bool media = false) {
  demo::SessionOptions value;
  value.room = "async-demo";
  value.connection.token = "secret-token";
  if (media) {
    MediaSpec spec;
    spec.demo = true;
    spec.width = spec.height = 16;
    value.media.push_back(spec);
  }
  return value;
}

void stop(demo::Session& session) {
  session.stop();
  until([&] { return session.snapshot().finished; }, "Stop waits for pending network completion",
        2s);
  check(session.snapshot().state == demo::State::Idle, "User stop returns session to idle");
}

void cancel_pending() {
  auto wire = std::make_shared<Wire>();
  wire->auto_join = false;
  demo::Session session(factory(wire));
  server_result(session.start(options()));
  until([&] { return wire->starts == 1; }, "Join submitted");
  stop(session);
  check(wire->stops == 1, "Pending join transport stopped");

  wire->auto_join = true;
  wire->hold_publish = true;
  server_result(session.start(options(true)));
  until([&] { return wire->publishes == 1; }, "Publish submitted");
  check(session.snapshot().state == demo::State::Connecting,
        "Startup waits for publish completion");
  stop(session);
  check(wire->stops == 2, "Pending publish transport stopped on rejoined session");
}

void initial_graph_failure() {
  auto wire = std::make_shared<Wire>();
  demo::Session session(factory(wire, false));
  server_result(session.start(options(true)));
  until([&] { return session.snapshot().finished; }, "Initial graph failure settles session");
  check(session.snapshot().state == demo::State::Error &&
            session.snapshot().error.find("Media is not enabled") != std::string::npos,
        "Rollback event cannot hide initial publication failure");
  check(wire->removes == 1 && wire->stops == 1, "Failed startup rolls back and closes");
}

void dynamic_video() {
  auto wire = std::make_shared<Wire>();
  demo::Session session(factory(wire));
  server_result(session.start(options(true)));
  until([&] { return session.snapshot().state == demo::State::Connected; }, "Initial media ready");
  check(session.remove_video("TR_local-1"), "Remove video queued");
  until([&] { return !session.snapshot().changing_video; }, "Remove completion delivered");
  check(wire->removes == 1 && session.snapshot().video_error.empty(), "One native removal");
  check(session.snapshot().state == demo::State::Connected, "track_removed does not remove twice");

  wire->reject_publish = true;
  check(session.add_video(), "Add video queued");
  until([&] { return !session.snapshot().changing_video; }, "Failed add completion delivered");
  auto state = session.snapshot();
  check(state.state == demo::State::Connected &&
            state.video_error == "publication rejected: [redacted]",
        "Dynamic failure preserves session and original redacted error");
  wire->reject_publish = false;
  check(session.add_video(), "Retry after failed add");
  until([&] { return !session.snapshot().changing_video; }, "Retried add completes");
  check(session.snapshot().video_error.empty(), "Retry succeeds");
  stop(session);
}

void remove_pending_subscription() {
  auto wire = std::make_shared<Wire>();
  wire->hold_subscribe = true;
  demo::Session session(factory(wire));
  std::atomic<int> reports{0};
  auto config = options();
  config.log_sink = [&](const Json& event) {
    if (event.value("event", "") == "media-graph-stats") {
      ++reports;
    }
    return Result<void>::success();
  };
  server_result(session.start(std::move(config)));
  until([&] { return session.snapshot().state == demo::State::Connected; }, "Subscriber joined");
  TrackDescriptor descriptor;
  descriptor.id = "remote";
  descriptor.remote_sid = "TR_remote";
  descriptor.format.width = descriptor.format.height = 16;
  TransportEvent added{TransportEventKind::remote_track_added};
  added.descriptor = descriptor;
  wire->push({added, added});
  until([&] { return wire->subscribes == 1; }, "Subscribe submitted once");
  wire->push({{TransportEventKind::remote_track_removed, wire->binding("remote")}});
  until([&] { return wire->removes == 1; }, "Pending subscribe can be removed without waiting");
  until([&] { return reports >= 2 || session.snapshot().finished; },
        "Session continues processing after pending removal");
  check(session.snapshot().state == demo::State::Connected,
        "Remote removal cancellation is not fatal");
  stop(session);
}
}  // namespace

int main() {
  cancel_pending();
  initial_graph_failure();
  dynamic_video();
  remove_pending_subscription();
  std::cout << "Conference async session contracts passed\n";
}
