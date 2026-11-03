#include "livekit/client/room.hpp"
#include "backends/livekit/room_session.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <type_traits>
using namespace livekit::sdk;
using namespace livekit::sdk::client;
using namespace std::chrono_literals;

namespace livekit::sdk::client::detail {
struct RoomTestAccess {
  static void use(Room& room, SessionFactory factory) {
    room.session_ = std::make_shared<RoomSession>(std::move(factory));
  }
};
}  // namespace livekit::sdk::client::detail

namespace {
namespace impl = livekit::sdk::client::detail;

// The internal unit result is explicit, while public compatibility stays bool/void.
static_assert(std::is_same_v<decltype(std::declval<impl::Operation>().status()), Result<void>>);
static_assert(std::is_same_v<decltype(std::declval<impl::RtcEngine>().connect(ConnectionOptions{})),
                             Result<void>>);
static_assert(std::is_same_v<decltype(std::declval<Room>().connect(ConnectionOptions{})), bool>);
static_assert(std::is_same_v<decltype(std::declval<LocalTrack>().set_muted(true)), void>);

void check(bool condition, const char* text) {
  if (!condition) {
    std::cerr << "FAIL: " << text << '\n';
    std::abort();
  }
}

template <class F>
void eventually(F predicate, const char* text) {
  auto deadline = SteadyClock::now() + 3s;
  while (!predicate()) {
    check(SteadyClock::now() < deadline, text);
    std::this_thread::sleep_for(2ms);
  }
}

struct State {
  std::atomic<bool> hold_connect{false}, hold_publish{false}, disconnect{false};
  std::atomic<int> connects{0}, publishes{0}, creates{0}, removes{0}, closes{0}, subscriptions{0};
  std::mutex mutex;
  std::vector<EncodedFrame> sent;
  std::vector<impl::SessionEvent> pending;
  std::weak_ptr<impl::TrackData> received;
  Failure sender_failure, subscription_failure;  // Guarded by mutex.
};

class FakeSession : public impl::RtcSession {
 public:
  explicit FakeSession(std::shared_ptr<State> state) : state_(std::move(state)) {}

  Result<impl::JoinState> connect(const ConnectionOptions&, Epoch epoch,
                                  const impl::Operation& op) override {
    ++state_->connects;
    epoch_ = epoch;
    while (state_->hold_connect) {
      if (auto error = op.status(); !error) {
        return Result<impl::JoinState>::failure(error.failure_record());
      }
      std::this_thread::sleep_for(2ms);
    }
    impl::JoinState join;
    join.room.name = "test";
    join.local.identity = "local";
    ParticipantSnapshot p;
    p.sid = "PA_remote";
    p.identity = "remote";
    p.state = 2;
    PublicationInfo track;
    track.sid = "TR_remote";
    track.kind = PublicationKind::audio;
    track.name = "audio";
    p.publications[track.sid] = track;
    join.participants.push_back(p);
    return Result<impl::JoinState>::success(join);
  }

  Result<void> poll(const impl::Operation&) override {
    std::vector<impl::SessionEvent> pending;
    {
      std::lock_guard lock(state_->mutex);
      pending.swap(state_->pending);
    }
    for (auto& event : pending) {
      events(std::move(event));
    }
    if (state_->disconnect.exchange(false)) {
      return Result<void>::failure(
          make_failure(ErrorDomain::transport, ErrorCode::connection_lost, "test restart", true));
    }
    return Result<void>::success();
  }

  void close() override {
    ++state_->closes;
    std::lock_guard lock(state_->mutex);
    state_->sent.clear();
  }

  Result<PublicationInfo> add_track(const impl::TrackConfig& config,
                                    const impl::Operation& op) override {
    ++state_->publishes;
    while (state_->hold_publish) {
      if (auto error = op.status(); !error) {
        return Result<PublicationInfo>::failure(error.failure_record());
      }
      std::this_thread::sleep_for(2ms);
    }
    PublicationInfo p;
    p.sid = "TR_" + std::to_string(epoch_) + "_" + config.id;
    p.name = config.name;
    return Result<PublicationInfo>::success(p);
  }

  Result<void> create_sender(const TrackSid&, std::shared_ptr<impl::TrackData> track,
                             const impl::Operation&) override {
    ++state_->creates;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->sender_failure) {
        return Result<void>::failure(state_->sender_failure);
      }
    }
    std::lock_guard lock(track->mutex);
    track->send = [state = state_](const EncodedFrame& frame) {
      std::lock_guard lock(state->mutex);
      state->sent.push_back(frame);
      return WriteStatus::accepted;
    };
    return Result<void>::success();
  }

  Result<void> remove_track(const TrackSid&, const impl::Operation&) override {
    ++state_->removes;
    return Result<void>::success();
  }

  Result<void> subscribe(const TrackSid& sid, std::shared_ptr<impl::TrackData> track, bool enabled,
                         const impl::Operation&) override {
    ++state_->subscriptions;
    {
      std::lock_guard lock(state_->mutex);
      if (enabled && state_->subscription_failure) {
        return Result<void>::failure(state_->subscription_failure);
      }
    }
    track->transition(enabled ? TrackState::ready : TrackState::suspended, epoch_, sid);
    std::lock_guard lock(state_->mutex);
    state_->received = enabled ? track : std::shared_ptr<impl::TrackData>{};
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

  Result<void> publish_data(const DataPacket& packet, const impl::Operation&) override {
    impl::SessionEvent event{impl::SessionEvent::Kind::data};
    event.data = packet;
    events(std::move(event));
    return Result<void>::success();
  }

  Result<void> update(const ParticipantUpdate&, const impl::Operation&) override {
    return Result<void>::success();
  }

 private:
  std::shared_ptr<State> state_;
  Epoch epoch_ = 0;
};

std::shared_ptr<State> fake(Room& room) {
  auto state = std::make_shared<State>();
  impl::RoomTestAccess::use(room, [state] { return std::make_unique<FakeSession>(state); });
  return state;
}

ConnectionOptions options() {
  ConnectionOptions o;
  o.url = "ws://fake";
  o.token = "fake";
  o.operation_timeout = 100ms;
  o.reconnect.initial_delay = 1ms;
  o.reconnect.max_delay = 10ms;
  return o;
}

LocalTrack audio(QueueLimits limits = {}) {
  CodecFormat f;
  f.codec = Codec::opus;
  f.clock_rate = 48000;
  f.channels = 1;
  auto track = LocalTrack::create_audio("test", f, limits);
  check(bool(track), "audio creation");
  return *track;
}

EncodedFrame frame(Epoch epoch, uint64_t timestamp = 0) {
  EncodedFrame f;
  f.buffer = EncodedBuffer({0xf8, 0xff, 0xfe});
  f.epoch = epoch;
  f.timestamp = timestamp;
  f.duration = 20ms;
  return f;
}

struct Observer : RoomObserver {
  Room* room = nullptr;
  std::atomic<int> count{0}, reentry{0};
  std::atomic<bool> hold{false}, entered{false};

  void on_room_event(const RoomEvent& event) override {
    ++count;
    if (event.kind == RoomEventKind::data_received) {
      entered = true;
      if (room) {
        auto p = room->local_participant().publish_track(audio());
        if (!p && p.error().code == ErrorCode::invalid_thread) {
          ++reentry;
        }
        check(room->connection_state() == ConnectionState::connected, "observer snapshot reads");
      }
      while (hold) {
        std::this_thread::sleep_for(2ms);
      }
    }
  }
};

void business_and_media() {
  Observer observer;
  Room room;
  observer.room = &room;
  auto state = fake(room);
  room.set_observer(&observer);
  check(room.connect(options()), "connect succeeds");
  auto local = room.local_participant();
  auto track = audio({2, 6, 1s});
  auto pub = local.publish_track(track);
  check(bool(pub), "publish returns publication");
  check(state->creates == 1 && state->publishes == 1,
        "participant performs AddTrack and sender creation");
  check(!local.publish_track(track), "track cannot be published twice");
  check(track.try_write(frame(99)) == WriteStatus::stale_epoch, "stale epoch rejected");
  check(track.try_write(frame(1)) == WriteStatus::accepted, "encoded frame accepted");
  check(track.try_write(frame(1, 960)) == WriteStatus::accepted, "second frame accepted");
  check(track.try_write(frame(1, 1920)) == WriteStatus::backpressure, "in-flight budget bounded");
  {
    std::lock_guard lock(state->mutex);
    state->sent.clear();
  }
  track.set_muted(true);
  check(track.try_write(frame(1, 1920)) == WriteStatus::not_ready, "mute gates media");
  track.set_muted(false);
  check(track.try_write(frame(1, 1920)) == WriteStatus::accepted, "unmute resumes media");
  eventually([&] { return state->subscriptions > 0; }, "auto subscribe");
  auto publication = room.remote_participants()[0].track_publications()[0];
  eventually([&] { return publication.track().has_value(); }, "remote track bound");
  auto remote = *publication.track();
  {
    std::lock_guard lock(state->mutex);
    state->received.lock()->receive(frame(1));
  }
  check(remote.read({10ms, {}}).status == ReadStatus::frame, "encoded receive queue");
  publication.set_subscribed(false);
  check(!publication.track(), "unsubscribe detaches handle");
  check(remote.read({10ms, {}}).status == ReadStatus::suspended, "old receive handle suspended");
  publication.set_subscribed(true);
  local.publish_data({{1}, true, "topic", {}});
  eventually([&] { return observer.reentry == 1; }, "observer synchronous reentry rejected");
  observer.hold = true;
  observer.entered = false;
  local.publish_data({{2}, true, "topic", {}});
  eventually([&] { return observer.entered.load(); }, "observer entered");
  std::atomic<bool> detached{false};
  std::thread detach([&] {
    room.set_observer(nullptr);
    detached = true;
  });
  std::this_thread::sleep_for(20ms);
  check(!detached, "observer unregister is a fence");
  observer.hold = false;
  detach.join();
  auto count = observer.count.load();
  local.publish_data({{3}, true, "topic", {}});
  std::this_thread::sleep_for(20ms);
  check(observer.count == count, "no notifications after unregister");
  auto old_sid = pub->snapshot().sid;
  publication.set_subscribed(false);
  ReceiveOptions demand;
  demand.disabled = true;
  demand.quality = ReceiveQuality::low;
  publication.set_receive_options(demand);
  state->disconnect = true;
  eventually(
      [&] {
        return track.snapshot().epoch > 1 && room.connection_state() == ConnectionState::connected;
      },
      "rejoin restores publication");
  check(pub->snapshot().sid != old_sid, "stable publication handle gets new SID");
  auto restored = room.remote_participants()[0].track_publications()[0];
  check(!restored.snapshot().desired && !restored.track(), "rejoin preserves unsubscribe intent");
  check(track.try_write(frame(1, 2880)) == WriteStatus::stale_epoch, "rejoin rejects old epoch");
  local.unpublish_track(pub->snapshot().sid);
  check(state->removes == 1, "unpublish delegates native removal");
  check(track.snapshot().state == TrackState::closed, "unpublish closes media");
  check(local.track_publications().empty(), "participant owns publication registry");
  room.close();
  check(room.connection_state() == ConnectionState::closed, "close returns closed");
}

void cancellation() {
  Room room;
  auto state = fake(room);
  state->hold_connect = true;
  std::atomic<bool> done{false}, connected{true};
  std::thread thread([&] {
    connected = room.connect(options());
    done = true;
  });
  eventually([&] { return state->connects > 0; }, "connect started");
  check(!done, "connect waits for readiness");
  room.close();
  thread.join();
  check(!connected, "close cancels connect");
  Room publish_room;
  auto publish = fake(publish_room);
  check(publish_room.connect(options()), "publish room connect");
  auto track = audio();
  publish->hold_publish = true;
  auto result = publish_room.local_participant().publish_track(track);
  check(!result && result.error().code == ErrorCode::timeout, "publish timeout propagated");
  publish->hold_publish = false;
  check(bool(publish_room.local_participant().publish_track(track)),
        "failed publish releases claim");
  Room other;
  fake(other);
  check(other.connect(options()), "other room connect");
  check(!other.local_participant().publish_track(track), "cross-room claim exclusive");
  auto receive_pub = publish_room.remote_participants()[0].track_publications()[0];
  eventually([&] { return receive_pub.track().has_value(); }, "receiver ready");
  auto receiver = *receive_pub.track();
  ReadResult received;
  std::thread reader([&] { received = receiver.read({5s, {}}); });
  publish->hold_publish = true;
  const auto before = publish->publishes.load();
  std::atomic<bool> cancelled{false};
  std::thread publishing([&] {
    auto result = publish_room.local_participant().publish_track(audio());
    cancelled = !result && result.error().code == ErrorCode::cancelled;
  });
  eventually([&] { return publish->publishes > before; }, "blocked publish started");
  publish_room.close();
  publishing.join();
  reader.join();
  check(cancelled, "close cancels blocked publish");
  check(received.status == ReadStatus::closed, "close interrupts blocked read");
}
}  // namespace

void reuse_after_failure() {
  Room first, second;
  auto state = fake(first);
  fake(second);
  auto config = options();
  config.reconnect.enabled = false;
  check(first.connect(config) && second.connect(options()), "rooms for reuse");
  auto track = audio();
  check(bool(first.local_participant().publish_track(track)), "first owner publishes");
  state->disconnect = true;
  eventually([&] { return first.connection_state() == ConnectionState::failed; },
             "first owner fails");
  check(bool(second.local_participant().publish_track(track)), "new owner claims released track");
  first.close();
  check(track.snapshot().state == TrackState::ready, "old room close cannot invalidate new owner");
  check(track.try_write(frame(track.feedback().epoch)) == WriteStatus::accepted,
        "new owner still sends");
}

void operation_results() {
  Room room;
  auto state = fake(room);
  auto config = options();
  config.auto_subscribe = false;
  check(room.connect(config), "successful internal result maps to true");
  auto failure = make_failure(ErrorDomain::transport, ErrorCode::request_failed,
                              "injected sender failure", false, 42);
  {
    std::lock_guard lock(state->mutex);
    state->sender_failure = failure;
  }
  auto track = audio();
  auto publication = room.local_participant().publish_track(track);
  check(!publication && publication.failure_record() == failure,
        "sender error retains original failure identity");
  check(publication.error().backend_reason == 42 && state->removes == 1,
        "sender failure rolls back acknowledged publication");
  check(room.local_participant().track_publications().empty(),
        "failed result never inserts publication");
  {
    std::lock_guard lock(state->mutex);
    state->sender_failure.reset();
    state->subscription_failure = failure;
  }
  check(bool(room.local_participant().publish_track(track)),
        "success result publishes after rollback");
  auto remote = room.remote_participants()[0].track_publications()[0];
  remote.set_subscribed(true);
  auto snapshot = remote.snapshot();
  check(snapshot.state == TrackState::failed && snapshot.failure == failure && !remote.track(),
        "void public operation retains internal error in publication snapshot");
  {
    std::lock_guard lock(state->mutex);
    state->subscription_failure.reset();
  }
  remote.set_subscribed(true);
  check(remote.snapshot().subscribed && !remote.snapshot().failure && remote.track().has_value(),
        "success clears previous subscription error");
}

void participant_media_status() {
  Room room;
  auto state = fake(room);
  check(room.connect(options()), "status room connects");
  impl::SessionEvent quality{impl::SessionEvent::Kind::quality};
  quality.participant.sid = "PA_remote";
  quality.participant.connection_quality = 2;
  impl::SessionEvent speakers{impl::SessionEvent::Kind::speakers};
  ParticipantSnapshot speaker;
  speaker.sid = "PA_remote";
  speaker.speaking = true;
  speaker.audio_level = .5f;
  speakers.speakers.push_back(speaker);
  {
    std::lock_guard lock(state->mutex);
    state->pending = {quality, speakers};
  }
  eventually(
      [&] {
        auto p = room.remote_participants()[0].snapshot();
        return p.connection_quality == 2 && p.speaking && p.audio_level == .5f;
      },
      "quality and active speaker update roster");
  speakers.speakers.clear();
  speakers.full_speaker_update = true;
  {
    std::lock_guard lock(state->mutex);
    state->pending.push_back(speakers);
  }
  eventually([&] { return !room.remote_participants()[0].snapshot().speaking; },
             "full speaker update clears absent speaker");
  check(room.remote_participants()[0].snapshot().connection_quality == 2,
        "speaker update preserves quality");
}

int main() {
  participant_media_status();
  operation_results();
  business_and_media();
  cancellation();
  reuse_after_failure();
  for (int i = 0; i < 20; ++i) {
    Room room;
    fake(room);
    check(room.connect(options()), "repeat connect");
    room.close();
  }
  std::cout << "PASS client ownership, synchronous operations, media bounds, observer fence, "
               "cancellation, rejoin\n";
}
