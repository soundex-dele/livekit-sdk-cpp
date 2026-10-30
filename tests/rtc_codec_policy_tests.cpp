#include "rtc_blocking_bridge.hpp"
#include "backends/rtc/room_transport.hpp"
#include <atomic>
#include <iostream>
#include <thread>

using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::engine;
using namespace livekit::sdk::rtc::detail;
using namespace livekit::sdk::app_support;
using namespace std::chrono_literals;

namespace {
void check(bool value, const char* message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

template <class F>
void until(F done) {
  const auto deadline = SteadyClock::now() + 8s;
  while (!done()) {
    check(SteadyClock::now() < deadline, "Codec policy operation timed out");
    std::this_thread::sleep_for(5ms);
  }
}

struct Wire {
  std::mutex mutex;
  std::shared_ptr<livekit::sdk::detail::WakeEvent> wake;
  std::vector<TransportEvent> events;
  SessionInfo session;
  std::map<TrackId, TrackDescriptor> published;
  std::map<TrackId, size_t> publishes, packets, keyframes;
  bool deny_update = false;

  void emit(TransportEvent event) {
    events.push_back(std::move(event));
    wake->signal();
  }

  void roster() {
    ++session.revision;
    TransportEvent event{TransportEventKind::session_changed, {"", session.epoch}};
    event.session = std::make_shared<SessionInfo>(session);
    emit(std::move(event));
  }

  void peer(std::optional<std::string> codecs, bool sticky = false) {
    std::lock_guard lock(mutex);
    session.remote.clear();
    if (codecs) {
      ParticipantSnapshot participant;
      participant.sid = "remote";
      participant.identity = "remote";
      participant.attributes[kVideoDecodeCodecsAttribute] = *codecs;
      participant.attributes[kVideoH264LatchAttribute] = sticky ? "1" : "0";
      session.remote[participant.sid] = participant;
    }
    roster();
  }

  bool codec(const TrackId& id, Codec expected) {
    std::lock_guard lock(mutex);
    return published.contains(id) && published.at(id).format.codec == expected;
  }
};

struct Transport final : RoomTransport {
  explicit Transport(std::shared_ptr<Wire> wire) : wire(std::move(wire)) {}

  std::shared_ptr<Wire> wire;

  BackendCapabilities capabilities() const override {
    BackendCapabilities result;
    result.participant_update_ack = true;
    return result;
  }

  void start(const RoomOptions&, Epoch epoch) override {
    std::lock_guard lock(wire->mutex);
    wire->wake = wakeup_;
    wire->session = {};
    wire->session.epoch = epoch;
    wire->session.local.sid = "local";
    wire->emit({TransportEventKind::connected});
    wire->roster();
  }

  void update_participant(ParticipantUpdateRequest request) override {
    std::lock_guard lock(wire->mutex);
    TransportEvent event{TransportEventKind::control_completed, {"", request.epoch}};
    event.operation_id = request.id;
    if (wire->deny_update) {
      event.control_error = ErrorCode::unsupported;
    } else {
      for (const auto& [key, value] : request.update.attributes) {
        wire->session.local.attributes[key] = value;
      }
      wire->roster();
    }
    wire->emit(std::move(event));
  }

  void publish(const BindingToken& binding, const TrackDescriptor& descriptor) override {
    std::lock_guard lock(wire->mutex);
    wire->published[binding.id] = descriptor;
    const auto version = ++wire->publishes[binding.id];
    wire->emit({TransportEventKind::published, binding,
                "sid/" + binding.id + "/" + std::to_string(version)});
  }

  void subscribe(const BindingToken&, const TrackDescriptor&,
                 std::shared_ptr<QueueBudget>) override {}

  void remove(const BindingToken& binding, bool, Removed completed) override {
    std::lock_guard lock(wire->mutex);
    wire->published.erase(binding.id);
    if (completed) {
      completed({});
    }
  }

  SendResult send(const BindingToken& binding, const EncodedFrame& frame) override {
    std::lock_guard lock(wire->mutex);
    ++wire->packets[binding.id];
    wire->keyframes[binding.id] += frame.keyframe;
    return SendResult::sent;
  }

  void request_keyframe(const BindingToken&) override {}

  void stop() noexcept override {}

  std::vector<TransportEvent> take_events() override {
    std::lock_guard lock(wire->mutex);
    return std::exchange(wire->events, {});
  }
};

MediaPublishSpec video(std::string id) {
  MediaPublishSpec spec;
  spec.track.id = std::move(id);
  spec.track.format.codec = Codec::h265;
  spec.track.format.width = 64;
  spec.track.format.height = 48;
  spec.input.generated = true;
  return spec;
}

RoomOptions options(bool h265 = true) {
  RoomOptions result;
  result.video_codec_policy = RoomVideoCodecPolicy{h265};
  result.operation_timeout = 1s;
  return result;
}

void switching() {
  auto wire = std::make_shared<Wire>();
  BlockingRtc engine([wire] { return std::make_unique<Transport>(wire); });
  require_result(engine.enable_media());
  require_result(engine.join(options()));
  wire->peer("h264,h265");
  auto first = require_result(engine.publish_video(video("first")));
  auto muted_spec = video("muted");
  muted_spec.track.muted = true;
  auto second = require_result(engine.publish_video(muted_spec));
  auto removed = require_result(engine.publish_video(video("removed")));
  require_result(engine.unpublish("removed"));
  until([&] { return first.track->snapshot().frames >= 3; });
  check(wire->codec("first", Codec::h265), "Compatible room downgraded prematurely");
  const auto old_sid = engine.publication("first").server_sid;
  wire->peer("h264");
  until([&] {
    // Transport publish records intent before ACK and graph installation. Wait
    // for both replacements and the policy transaction, then inspect mute state.
    return wire->codec("first", Codec::h264) && wire->codec("muted", Codec::h264) &&
           engine.publication("first").consumed >= 3 &&
           engine.publication("muted").state == TrackState::ready &&
           !engine.raw().video_codec_policy().transitioning;
  });
  check(first.track->snapshot().transport.state == TrackState::closed,
        "Old publication handle remained live");
  check(engine.publication("first").server_sid != old_sid, "Replacement reused old SID");
  check(engine.publication("muted").muted, "Replacement lost mute intent");
  check(!wire->codec("removed", Codec::h264), "Removed publication resurrected");
  check(engine.raw().video_codec_policy().h264_latched, "Missing sticky room decision");
  {
    std::lock_guard lock(wire->mutex);
    check(wire->session.local.attributes[kVideoH264LatchAttribute] == "1",
          "Fallback was not propagated to future participants");
    check(wire->keyframes["first"] >= 2, "Replacement did not send a new keyframe");
  }
  wire->peer(std::nullopt);
  require_result(engine.publish_video(video("late")));
  check(wire->codec("late", Codec::h264), "Late publication ignored sticky policy");
  // Repeated notifications and a later H265-only declaration cannot undo the latch.
  wire->peer("h264,h265");
  require_result(engine.publish_video(video("later")));
  {
    std::lock_guard lock(wire->mutex);
    check(wire->publishes["first"] == 2, "Duplicate roster notification repeated replacement");
  }
  const auto epoch = engine.room().snapshot().epoch;
  {
    std::lock_guard lock(wire->mutex);
    wire->emit({TransportEventKind::interrupted});
    wire->emit({TransportEventKind::connected});
  }
  until([&] { return engine.room().snapshot().epoch > epoch; });
  // Recovery can precede the new roster. Lost attributes must be re-advertised.
  std::this_thread::sleep_for(50ms);
  {
    std::lock_guard lock(wire->mutex);
    wire->session.epoch = epoch + 1;
    wire->session.local.attributes.clear();
    wire->roster();
  }
  until([&] {
    std::lock_guard lock(wire->mutex);
    return wire->session.local.attributes[kVideoH264LatchAttribute] == "1";
  });
  check(wire->codec("first", Codec::h264) && !engine.raw().video_codec_policy().failure,
        "Recovery lost codec decision or treated the pending roster as failure");
  require_result(engine.leave());
  require_result(engine.join(options()));
  require_result(engine.publish_video(video("fresh-room")));
  check(wire->codec("fresh-room", Codec::h265), "Explicit leave did not reset the policy");
  require_result(engine.close());
}

void unknown_and_late_join() {
  for (const auto& [codecs, sticky] :
       {std::pair{std::string{}, false}, std::pair{std::string{"h264,h265"}, true}}) {
    auto wire = std::make_shared<Wire>();
    BlockingRtc engine([wire] { return std::make_unique<Transport>(wire); });
    require_result(engine.enable_media());
    require_result(engine.join(options()));
    wire->peer(codecs, sticky);
    until([&] { return engine.raw().video_codec_policy().h264_latched; });
    require_result(engine.publish_video(video("first")));
    check(wire->codec("first", Codec::h264), "Unknown peer or remote latch was ignored");
    require_result(engine.close());
  }
}

void encoded_and_errors() {
  struct Replacements final : engine::PublicationObserver {
    std::mutex mutex;
    std::map<TrackId, Result<MediaPublication>> results;

    void on_video_codec_republished(const TrackId& id, Result<MediaPublication> result) override {
      std::lock_guard lock(mutex);
      results.insert_or_assign(id, std::move(result));
    }
  };

  auto wire = std::make_shared<Wire>();
  BlockingRtc engine([wire] { return std::make_unique<Transport>(wire); });
  require_result(engine.enable_media());
  require_result(engine.join(options()));
  // Unmanaged graphs cannot promise to replace their external codec.
  auto rejected = engine.publish(video("unmanaged").track);
  check(!rejected && rejected.error().code == ErrorCode::unsupported,
        "Unmanaged H265 bypassed room policy");
  EncodedSourceOptions input;
  input.codec = "h265";
  input.width = 64;
  input.height = 48;
  auto spec = video("encoded");
  spec.input.generated = false;
  spec.input.encoded_source = std::make_shared<EncodedMediaSource>(input);
  require_result(engine.publish_media(spec));
  auto replacement = spec;
  replacement.track.id = "replaceable";
  replacement.input.encoded_source = std::make_shared<EncodedMediaSource>(input);
  std::atomic<int> calls = 0;
  replacement.h264_fallback_input = [&] {
    ++calls;
    MediaInput raw;
    raw.generated = true;
    return raw;
  };
  require_result(engine.publish_media(replacement));
  auto completions = std::make_shared<Replacements>();
  engine.raw().set_publication_observer(completions);
  wire->peer("h264");
  until([&] {
    return wire->codec("replaceable", Codec::h264) && engine.raw().video_codec_policy().failure;
  });
  check(engine.publication("encoded").id.empty(), "Unsupported encoded H265 kept sending");
  check(calls == 1, "Encoded source replacement was not invoked once");
  check(engine.raw().video_codec_policy().failure->code == ErrorCode::unsupported,
        "Missing explicit unsupported-source failure");
  until([&] {
    std::lock_guard lock(completions->mutex);
    return completions->results.size() == 2;
  });
  {
    std::lock_guard lock(completions->mutex);
    check(!completions->results.at("encoded") && completions->results.at("replaceable"),
          "Replacement observers did not receive individual results");
    check(completions->results.at("replaceable")->track->snapshot().transport.state ==
              TrackState::ready,
          "Successful replacement callback returned an unusable handle");
  }
  require_result(engine.close());

  wire = std::make_shared<Wire>();
  wire->deny_update = true;
  BlockingRtc denied([wire] { return std::make_unique<Transport>(wire); });
  check(denied.join(options()) != ErrorCode::ok, "Missing metadata permission was ignored");
  check(denied.room().snapshot().state == ConnectionState::closed,
        "Failed policy initialization left room active");
  require_result(denied.close());
}
}  // namespace

int main() {
  try {
    switching();
    unknown_and_late_join();
    encoded_and_errors();
    std::cout << "PASS sticky H264 policy, graph replacements, mute, late publish, unknown peers, "
                 "remote latch, encoded input and failure contracts\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
