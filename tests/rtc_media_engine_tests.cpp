#include "livekit/rtc/livekit.hpp"
#include "livekit/rtc/engine.hpp"
#include "backends/rtc/room_transport.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>
#include <utility>

using livekit::sdk::AsyncResult;
using livekit::sdk::CompletionSource;
using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::engine;
using namespace livekit::sdk::rtc::detail;
using namespace std::chrono_literals;

namespace {
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

struct Wire {
  std::mutex mutex;
  std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup;
  std::vector<TransportEvent> events;
  std::map<TrackId, BindingToken> subscribers;
  std::map<TrackId, BindingToken> publishers;
  uint64_t feedback_revision = 0;
  std::map<TrackId, size_t> packets;
  std::atomic<bool> acknowledge{true};
  std::atomic<int> starts{0}, stops{0};

  void emit(TransportEvent event) {
    std::lock_guard lock(mutex);
    events.push_back(std::move(event));
    if (wakeup) {
      wakeup->signal();
    }
  }
};

class Transport final : public RoomTransport {
 public:
  explicit Transport(std::shared_ptr<Wire> wire) : w(std::move(wire)) {}

  void start(const RoomOptions&, Epoch) override {
    ++w->starts;
    {
      std::lock_guard lock(w->mutex);
      w->wakeup = wakeup_;
    }
    w->emit({TransportEventKind::connected});
  }

  void stop() noexcept override {
    ++w->stops;
  }

  void publish(const BindingToken& binding, const TrackDescriptor&) override {
    {
      std::lock_guard lock(w->mutex);
      w->publishers[binding.id] = binding;
    }
    if (w->acknowledge)
      w->emit({TransportEventKind::published, binding, "SID/" + binding.id});
  }

  void subscribe(const BindingToken& binding, const TrackDescriptor& d,
                 std::shared_ptr<QueueBudget>) override {
    {
      std::lock_guard lock(w->mutex);
      w->subscribers[d.remote_sid.substr(4)] = binding;
    }
    w->emit({TransportEventKind::subscribed, binding, d.remote_sid});
  }

  AsyncResult<void> remove(const BindingToken& binding, bool) override {
    std::lock_guard lock(w->mutex);
    std::erase_if(w->subscribers, [&](const auto& item) { return item.second == binding; });
    std::erase_if(w->publishers, [&](const auto& item) { return item.second == binding; });
    CompletionSource<void> done;
    done.set_value();
    return done.get_result();
  }

  SendResult send(const BindingToken& binding, const EncodedFrame& frame) override {
    std::lock_guard lock(w->mutex);
    ++w->packets[binding.id];
    if (auto it = w->subscribers.find(binding.id); it != w->subscribers.end()) {
      TransportEvent event{TransportEventKind::frame, it->second};
      event.frame = frame;
      w->events.push_back(std::move(event));
      wakeup_->signal();
    }
    return SendResult::sent;
  }

  void request_keyframe(const BindingToken& binding) override {
    std::lock_guard lock(w->mutex);
    for (const auto& [id, subscriber] : w->subscribers) {
      if (subscriber != binding || !w->publishers.contains(id))
        continue;
      const auto publisher = w->publishers.at(id);
      SenderFeedback feedback;
      feedback.epoch = publisher.epoch;
      feedback.revision = ++w->feedback_revision;
      auto& encoding = feedback.encodings[""];
      encoding.active = true;
      encoding.target_bitrate_bps = 2000000;
      encoding.keyframe_required = true;
      encoding.keyframe_requests = feedback.revision;
      TransportEvent event{TransportEventKind::feedback, publisher};
      event.feedback = feedback;
      w->events.push_back(std::move(event));
      wakeup_->signal();
    }
  }

  std::vector<TransportEvent> take_events() override {
    std::lock_guard lock(w->mutex);
    return std::exchange(w->events, {});
  }

 private:
  std::shared_ptr<Wire> w;
};

template <class F>
void until(RtcEngine& engine, F done) {
  const auto deadline = SteadyClock::now() + 5s;
  while (!done()) {
    check(SteadyClock::now() < deadline, "Media engine operation timed out");

    std::this_thread::sleep_for(1ms);
  }
}

template <class T>
auto await(RtcEngine& engine, AsyncResult<T>& operation) {
  until(engine, [&] { return operation.wait_for(0ms) == std::future_status::ready; });
  return operation.get();
}

MediaPublishSpec video(std::string id) {
  MediaPublishSpec spec;
  spec.track.id = std::move(id);
  spec.track.format.codec = Codec::vp8;
  spec.track.format.width = 64;
  spec.track.format.height = 48;
  spec.input.generated = true;
  return spec;
}

std::unique_ptr<RtcEngine> make_engine(std::shared_ptr<Wire> wire) {
  auto result = std::make_unique<RtcEngine>([wire] { return std::make_unique<Transport>(wire); });
  result->enable_media().get();
  LiveKitRoomOptions options;
  options.url = "ws://test";
  options.token = "test";
  auto joined = result->join(options);
  await(*result, joined);
  return result;
}

void multi_track() {
  auto wire = std::make_shared<Wire>();
  auto engine = make_engine(wire);
  auto first = engine->publish_video(video("first")),
       second = engine->publish_video(video("second"));
  await(*engine, first.ready);
  await(*engine, second.ready);
  std::atomic<int> frames = 0;
  MediaSubscribeSpec config;
  config.track = video("rx").track;
  config.track.remote_sid = "SID/first";
  config.output.video = [&](const VideoSample& frame) {
    check(frame.pixels->width == 64 && frame.pixels->height == 48, "Wrong decoded dimensions");
    ++frames;
  };
  auto rx = engine->subscribe_media(config);
  await(*engine, rx.ready);
  until(*engine, [&] { return frames >= 3 && second.track->snapshot().frames >= 3; });
  const auto graph = first.track->snapshot();
  check(graph.video && graph.video->completed > 0 && graph.edges.size() == 4,
        "Engine did not build codec and feedback graph");
  engine->unpublish("first").get();
  engine->unsubscribe("rx").get();
  const auto received = frames.load();
  const auto before = second.track->snapshot().frames;
  until(*engine, [&] { return second.track->snapshot().frames >= before + 3; });
  check(frames == received && wire->starts == 1 && wire->stops == 0,
        "Closing one graph disturbed room or callback barrier");
  engine->close().get();
  check(wire->stops == 1, "Media engine failed to close shared transport");
}

void cancelled_and_failed_sources() {
  auto wire = std::make_shared<Wire>();
  wire->acknowledge = false;
  auto engine = make_engine(wire);
  livekit::sdk::I420SourceOptions options;
  options.width = 64;
  options.height = 48;
  auto source = std::make_shared<livekit::sdk::I420VideoSource>(options);
  auto spec = video("cancelled");
  spec.input.generated = false;
  spec.input.video_source = source;
  auto pending = engine->publish_video(spec);
  check(source->stats().attached, "Engine did not lease input");
  engine->unpublish(spec.track.id).get();
  bool cancelled = false;
  try {
    pending.ready.get();
  } catch (...) {
    cancelled = true;
  }
  check(cancelled && !source->stats().attached && !source->stats().closed,
        "Cancelled publication leaked or closed application source");
  check(pending.track->snapshot().transport.state == TrackState::failed,
        "Cancelled operation still appears pending");
  spec.track.id = "backend-rejected";
  auto rejected = engine->publish_video(spec);
  BindingToken rejected_binding;
  until(*engine, [&] {
    std::lock_guard lock(wire->mutex);
    auto found = wire->publishers.find(spec.track.id);
    if (found == wire->publishers.end()) {
      return false;
    }
    rejected_binding = found->second;
    return true;
  });
  TransportEvent failure{TransportEventKind::operation_failed, rejected_binding};
  failure.error = "publication rejected";
  wire->emit(std::move(failure));
  bool backend_failed = false;
  try {
    await(*engine, rejected.ready);
  } catch (...) {
    backend_failed = true;
  }
  check(backend_failed && !source->stats().attached && !source->stats().closed &&
            rejected.track->snapshot().transport.state == TrackState::failed,
        "Backend failure became ready before media cleanup");
  wire->acknowledge = true;
  spec.track.id = "bad-codec";
  spec.video.max_frames = 0;
  auto bad = engine->publish_video(spec);
  bool failed = false;
  try {
    await(*engine, bad.ready);
  } catch (...) {
    failed = true;
  }
  check(failed && !source->stats().attached && !source->stats().closed,
        "Graph startup failure leaked input lease");
  spec.track.id = "replacement";
  spec.video.max_frames = 2;
  auto replacement = engine->publish_video(spec);
  await(*engine, replacement.ready);
  engine->close().get();
  check(!source->stats().attached && !source->stats().closed,
        "Close destroyed application-owned source");
}

void raw_audio_and_encoded() {
  auto wire = std::make_shared<Wire>();
  auto engine = make_engine(wire);
  auto spec = video("audio");
  spec.track.kind = MediaKind::audio;
  spec.track.format = {};
  spec.track.format.codec = Codec::opus;
  spec.track.format.clock_rate = 48000;
  spec.track.format.channels = 1;
  auto tx = engine->publish_audio(spec);
  await(*engine, tx.ready);
  std::atomic<int> samples = 0;
  MediaSubscribeSpec sub;
  sub.track = spec.track;
  sub.track.id = "audio-rx";
  sub.track.remote_sid = "SID/audio";
  sub.output.audio = [&](const AudioSample& value) {
    check(value.pcm->sample_rate == 48000, "Wrong PCM rate");
    samples += int(value.pcm->samples_per_channel());
  };
  auto rx = engine->subscribe_media(sub);
  await(*engine, rx.ready);
  until(*engine, [&] { return samples >= 1920; });
  auto passthrough = video("encoded");
  passthrough.input.generated = false;
  livekit::sdk::EncodedSourceOptions options;
  options.codec = "vp8";
  options.width = 64;
  options.height = 48;
  passthrough.input.encoded_source = std::make_shared<livekit::sdk::EncodedMediaSource>(options);
  auto encoded = engine->publish_media(passthrough);
  await(*engine, encoded.ready);
  const auto graph = encoded.track->snapshot();
  check(!graph.video && !graph.audio && graph.edges.size() == 2, "Encoded input created a codec");
  engine->close().get();
}

void shared_capture() {
  auto wire = std::make_shared<Wire>();
  auto engine = make_engine(wire);
  auto other = make_engine(std::make_shared<Wire>());
  const auto format = video("capture");
  auto shared = engine->create_shared_input(format.track, format.input).get();
  auto a = video("shared-a"), b = video("shared-b");
  a.input = {};
  b.input = {};
  a.shared_input = shared;
  b.shared_input = shared;
  auto first = engine->publish_video(a), second = other->publish_video(b);
  await(*engine, first.ready);
  await(*other, second.ready);
  until(*engine, [&] {
    return first.track->snapshot().frames >= 3 && second.track->snapshot().frames >= 3;
  });
  check(shared->snapshot().consumers == 2 && shared->snapshot().running,
        "Shared capture was not activated once for two consumers");
  engine->leave().get();
  check(shared->snapshot().consumers == 1 && shared->snapshot().running,
        "One publication stopped shared capture");
  const auto previous = second.track->snapshot().frames;
  until(*engine, [&] { return second.track->snapshot().frames >= previous + 3; });
  other->unpublish(b.track.id).get();
  check(!shared->snapshot().running && shared->snapshot().consumers == 0,
        "Last publication leaked capture");
  LiveKitRoomOptions room;
  room.url = "ws://test";
  room.token = "test";
  auto joining = engine->join(room);
  await(*engine, joining);
  auto replacement = engine->publish_video(a);
  await(*engine, replacement.ready);
  until(*engine, [&] { return replacement.track->snapshot().frames >= 2; });
  engine->close().get();
  check(!shared->snapshot().running, "Reusable source survived room close as active capture");
  other->close().get();
}

void shared_audio() {
  auto engine = make_engine(std::make_shared<Wire>());
  livekit::sdk::PcmSourceOptions options;
  options.sample_rate = 44100;
  options.channels = 2;
  auto source = std::make_shared<livekit::sdk::PcmAudioSource>(options);
  MediaPublishSpec a;
  a.track.id = "pcm-a";
  a.track.kind = MediaKind::audio;
  a.track.format.codec = Codec::opus;
  a.track.format.clock_rate = 48000;
  a.track.format.channels = 2;
  MediaInput input;
  input.audio_source = source;
  auto shared = engine->create_shared_input(a.track, input).get();
  a.shared_input = shared;
  auto b = a;
  b.track.id = "pcm-b";
  std::atomic<int> first_blocks = 0, second_blocks = 0;
  const auto count = [](const AudioSample& sample, std::atomic<int>& blocks) {
    check(sample.pcm->sample_rate == 44100 && sample.pcm->channels == 2,
          "Shared PCM changed raw format");
    if (std::any_of(sample.pcm->samples.begin(), sample.pcm->samples.end(),
                    [](int16_t s) { return s != 0; }))
      ++blocks;
  };
  a.preview.audio = [&](const auto& sample) { count(sample, first_blocks); };
  b.preview.audio = [&](const auto& sample) { count(sample, second_blocks); };
  auto first = engine->publish_audio(a), second = engine->publish_audio(b);
  await(*engine, first.ready);
  await(*engine, second.ready);
  livekit::sdk::PcmInputFrame frame;
  frame.samples.assign(882, 8000);
  until(*engine, [&] {
    source->write(frame);
    return first_blocks >= 4 && second_blocks >= 4;
  });
  auto invalid = a;
  invalid.track.id = "bad-shared";
  invalid.audio.max_frames = 0;
  bool failed = false;
  try {
    auto bad = engine->publish_audio(invalid);
    await(*engine, bad.ready);
  } catch (...) {
    failed = true;
  }
  check(failed && shared->snapshot().consumers == 2, "Failed graph disturbed active shared input");
  engine->unpublish(a.track.id).get();
  const auto before = first_blocks.load(), remaining = second_blocks.load();
  until(*engine, [&] {
    source->write(frame);
    return second_blocks >= remaining + 4;
  });
  check(first_blocks == before && source->stats().attached,
        "Closing audio branch broke callback barrier or shared reader");
  engine->close().get();
  check(!source->stats().attached && !source->stats().closed,
        "Shared audio closed or leaked external source");
}
void media_leave_and_rejoin() {
  auto wire = std::make_shared<Wire>();
  auto rtc = make_engine(wire);
  auto spec = video("reusable");
  std::atomic<int> callbacks = 0;
  spec.preview.video = [&](const auto&) { ++callbacks; };
  auto active = rtc->publish_video(spec);
  await(*rtc, active.ready);
  until(*rtc, [&] { return callbacks >= 2; });
  wire->acknowledge = false;
  auto pending = rtc->publish_video(video("pending"));
  rtc->leave().get();
  const auto stopped = callbacks.load();
  bool cancelled = false;
  try {
    pending.ready.get();
  } catch (const std::exception&) {
    cancelled = true;
  }
  check(cancelled, "Leave failed to settle media operation");
  check(active.track->snapshot().transport.state == TrackState::closed,
        "Leave retained active media endpoint");
  wire->acknowledge = true;
  LiveKitRoomOptions room;
  room.url = "ws://test";
  room.token = "test";
  auto joining = rtc->join(room);
  await(*rtc, joining);
  check(callbacks == stopped, "Media callback escaped leave barrier");
  auto resumed = rtc->publish_video(spec);
  await(*rtc, resumed.ready);
  until(*rtc, [&] { return callbacks >= stopped + 2; });
  check(resumed.track->snapshot().transport.epoch > active.track->snapshot().transport.epoch,
        "Rejoined media reused the old epoch");
  rtc->close().get();
  rtc->close().get();
}

}  // namespace

int main() {
  try {
    std::cout << "multi_track" << std::endl;
    multi_track();
    media_leave_and_rejoin();
    std::cout << "cancelled_and_failed_sources" << std::endl;
    cancelled_and_failed_sources();
    std::cout << "raw_audio_and_encoded" << std::endl;
    raw_audio_and_encoded();
    std::cout << "shared_capture" << std::endl;
    shared_capture();
    std::cout << "shared_audio" << std::endl;
    shared_audio();
    std::cout << "media engine contracts passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
