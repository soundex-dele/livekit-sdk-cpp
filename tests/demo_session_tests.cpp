#include "server_result.hpp"
#include "session.hpp"
#include "backends/livekit/room_session.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>

using namespace livekit::sdk;
using namespace livekit::sdk::client;
using namespace std::chrono_literals;
namespace impl = livekit::sdk::client::detail;

namespace livekit::sdk::client::detail {
struct RoomTestAccess {
  static void use(Room& room, SessionFactory factory) {
    room.session_ = std::make_shared<RoomSession>(std::move(factory));
  }
};

struct MediaEngineTestAccess {
  static std::unique_ptr<MediaEngine> create(std::unique_ptr<Room> room) {
    return std::unique_ptr<MediaEngine>(new MediaEngine(std::move(room), {}));
  }
};
}  // namespace livekit::sdk::client::detail

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
  std::vector<impl::SessionEvent> pending;
  std::atomic<int> starts{0}, stops{0}, publishes{0}, subscribes{0}, removes{0};
  std::atomic<bool> hold_connect{false}, hold_publish{false}, hold_subscribe{false};
  std::atomic<bool> reject_publish{false};

  void advertise(bool removed = false) {
    impl::SessionEvent event{impl::SessionEvent::Kind::participant};
    event.participant.sid = "PA_remote";
    event.participant.identity = "remote";
    event.participant.state = 2;
    if (!removed) {
      PublicationInfo info;
      info.sid = "TR_remote";
      info.name = "remote-video";
      info.kind = PublicationKind::video;
      info.mime_type = "video/VP8";
      info.width = info.height = 16;
      event.participant.publications[info.sid] = info;
    }
    std::lock_guard lock(mutex);
    pending.push_back(std::move(event));
  }
};

class Transport final : public impl::RtcSession {
 public:
  explicit Transport(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}

  Result<impl::JoinState> connect(const ConnectionOptions&, Epoch epoch,
                                  const impl::Operation& op) override {
    ++wire_->starts;
    epoch_ = epoch;
    auto result = wait(wire_->hold_connect, op);
    if (!result) {
      return Result<impl::JoinState>::failure(result.failure_record());
    }
    impl::JoinState state;
    state.room.name = "sync-demo";
    state.local.sid = "PA_local";
    state.local.identity = "local";
    return Result<impl::JoinState>::success(state);
  }

  Result<void> poll(const impl::Operation&) override {
    std::vector<impl::SessionEvent> pending;
    {
      std::lock_guard lock(wire_->mutex);
      pending.swap(wire_->pending);
    }
    for (auto& event : pending) {
      if (events) {
        events(std::move(event));
      }
    }
    return Result<void>::success();
  }

  void close() override {
    if (!closed_) {
      ++wire_->stops;
      closed_ = true;
    }
  }

  Result<PublicationInfo> add_track(const impl::TrackConfig& config,
                                    const impl::Operation& op) override {
    ++wire_->publishes;
    auto result = wait(wire_->hold_publish, op);
    if (!result) {
      return Result<PublicationInfo>::failure(result.failure_record());
    }
    if (wire_->reject_publish) {
      return Result<PublicationInfo>::failure(make_failure(
          ErrorDomain::transport, ErrorCode::request_failed, "publication rejected: secret-token"));
    }
    PublicationInfo info;
    info.sid = "TR_" + config.id;
    info.name = config.name;
    info.kind = config.kind == MediaKind::video ? PublicationKind::video : PublicationKind::audio;
    return Result<PublicationInfo>::success(info);
  }

  Result<void> create_sender(const TrackSid&, std::shared_ptr<impl::TrackData> track,
                             const impl::Operation&) override {
    std::lock_guard lock(track->mutex);
    track->send = [](const EncodedFrame&) { return WriteStatus::accepted; };
    return Result<void>::success();
  }

  Result<void> remove_track(const TrackSid&, const impl::Operation&) override {
    ++wire_->removes;
    return Result<void>::success();
  }

  Result<void> subscribe(const TrackSid& sid, std::shared_ptr<impl::TrackData> track, bool enabled,
                         const impl::Operation& op) override {
    if (!enabled) {
      if (subscriptions_.erase(sid)) {
        ++wire_->removes;
      }
      return Result<void>::success();
    }
    subscriptions_.insert(sid);
    ++wire_->subscribes;
    while (wire_->hold_subscribe) {
      if (auto result = op.status(); !result) {
        return result;
      }
      poll(op);
      if (track->snapshot().state == TrackState::closed) {
        return Result<void>::failure(
            make_failure(ErrorDomain::media, ErrorCode::stale_handle, "Remote removed"));
      }
      std::this_thread::sleep_for(2ms);
    }
    track->transition(TrackState::ready, epoch_, sid);
    return Result<void>::success();
  }

  Result<void> receive_options(const TrackSid&, ReceiveOptions) override {
    return Result<void>::success();
  }

  Result<void> mute(const TrackSid&, bool) override {
    return Result<void>::success();
  }

  Result<void> keyframe(const TrackSid&) override {
    return Result<void>::success();
  }

  Result<void> publish_data(const DataPacket&, const impl::Operation&) override {
    return Result<void>::success();
  }

  Result<void> update(const ParticipantUpdate&, const impl::Operation&) override {
    return Result<void>::success();
  }

 private:
  Result<void> wait(const std::atomic<bool>& held, const impl::Operation& op) {
    while (held) {
      if (auto result = op.status(); !result) {
        return result;
      }
      std::this_thread::sleep_for(2ms);
    }
    return op.status();
  }

  std::set<TrackSid> subscriptions_;
  bool closed_ = false;
  Epoch epoch_ = 0;
  std::shared_ptr<Wire> wire_;
};

demo::Session::EngineFactory factory(std::shared_ptr<Wire> wire) {
  return [wire] {
    auto room = std::make_unique<Room>();
    impl::RoomTestAccess::use(*room, [wire] { return std::make_unique<Transport>(wire); });
    return Result<std::unique_ptr<MediaEngine>>::success(
        impl::MediaEngineTestAccess::create(std::move(room)));
  };
}

demo::SessionOptions options(bool media = false) {
  demo::SessionOptions value;
  value.room = "sync-demo";
  value.connection.url = "ws://fake";
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
  until([&] { return session.snapshot().finished; }, "Stop cancels pending synchronous operation",
        2s);
  check(session.snapshot().state == demo::State::Idle, "User stop returns session to idle");
}

void cancel_pending() {
  auto wire = std::make_shared<Wire>();
  wire->hold_connect = true;
  demo::Session session(factory(wire));
  server_result(session.start(options()));
  until([&] { return wire->starts == 1; }, "Connect submitted");
  stop(session);
  check(wire->stops == 1, "Pending connect stopped");
  wire->hold_connect = false;
  wire->hold_publish = true;
  server_result(session.start(options(true)));
  until([&] { return wire->publishes == 1; }, "Publish submitted");
  check(session.snapshot().state == demo::State::Connecting, "Startup waits for publish");
  stop(session);
  check(wire->stops == 2, "Pending publish stopped");
  wire->hold_publish = false;
  wire->hold_subscribe = true;
  server_result(session.start(options()));
  until([&] { return session.snapshot().state == demo::State::Connected; }, "Subscriber connected");
  wire->advertise();
  until([&] { return wire->subscribes == 1; }, "Subscribe submitted");
  stop(session);
}

void initial_graph_failure() {
  auto wire = std::make_shared<Wire>();
  demo::Session session(factory(wire));
  auto config = options(true);
  config.media[0].width = -1;
  server_result(session.start(config));
  until([&] { return session.snapshot().finished; }, "Invalid media settles session");
  check(session.snapshot().state == demo::State::Error && !session.snapshot().error.empty(),
        "Startup failure preserved");
  check(wire->stops == 1, "Failed startup closes");
}

void dynamic_video() {
  auto wire = std::make_shared<Wire>();
  demo::Session session(factory(wire));
  server_result(session.start(options(true)));
  until([&] { return session.snapshot().state == demo::State::Connected; }, "Initial media ready");
  const auto state = session.snapshot();
  const auto local = state.participants.find(state.local_sid);
  check(local != state.participants.end() && local->second.tracks.size() == 1,
        "Local publication visible");
  check(session.remove_video(local->second.tracks[0].sid), "Remove video queued");
  until([&] { return !session.snapshot().changing_video; }, "Remove finished");
  check(wire->removes == 1 && session.snapshot().video_error.empty(), "One removal");
  wire->reject_publish = true;
  check(session.add_video(), "Add queued");
  until([&] { return !session.snapshot().changing_video; }, "Failed add finished");
  check(session.snapshot().state == demo::State::Connected &&
            session.snapshot().video_error == "publication rejected: [redacted]",
        "Dynamic failure redacted");
  wire->reject_publish = false;
  check(session.add_video(), "Retry queued");
  until([&] { return !session.snapshot().changing_video; }, "Retry finished");
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
  server_result(session.start(config));
  until([&] { return session.snapshot().state == demo::State::Connected; }, "Subscriber joined");
  wire->advertise();
  wire->advertise();
  until([&] { return wire->subscribes == 1; }, "Subscribe submitted once");
  wire->advertise(true);
  until([&] { return wire->removes == 1; }, "Pending subscription removed");
  until([&] { return reports >= 2 || session.snapshot().finished; },
        "Session continues after removal");
  check(session.snapshot().state == demo::State::Connected, "Remote removal is not fatal");
  stop(session);
}
}  // namespace

int main() {
  cancel_pending();
  initial_graph_failure();
  dynamic_video();
  remove_pending_subscription();
  std::cout << "Conference synchronous session contracts passed\n";
}
