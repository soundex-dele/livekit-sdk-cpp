#include "livekit/rtc/engine.hpp"
#include "rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "livekit/rtc/graph_api.hpp"
#include "livekit/rtc/frame_view.hpp"
#include "livekit/media_core/frame.hpp"
#include "backends/rtc/room_transport.hpp"
#include "media_graph/raw_frame_queue.hpp"
#include "media_graph/sync_clock.hpp"
#include "media_nodes/capture/audio_processor.hpp"
#include "media_nodes/playout/audio_output_driver.hpp"
#include "engines/rtc/application_output.hpp"
#include "engines/rtc/media_graph_builder.hpp"
#include "rtc_audio_fixtures.hpp"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <thread>

using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::engine;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <class Predicate>
void eventually(Predicate predicate) {
  const auto deadline = SteadyClock::now() + 5s;
  while (!predicate()) {
    check(SteadyClock::now() < deadline, "Timed out waiting for architecture contract");
    std::this_thread::sleep_for(1ms);
  }
}

class LocalTransport final : public detail::RoomTransport {
 public:
  void start(const RoomOptions&, Epoch) override {
    events_.push_back({detail::TransportEventKind::connected});
    wakeup_->signal();
  }

  std::vector<detail::TransportEvent> take_events() override {
    return std::exchange(events_, {});
  }

  void publish(const detail::BindingToken& token, const TrackDescriptor&) override {
    if (token.id != "pending") {
      events_.push_back({detail::TransportEventKind::published, token, "sid"});
      wakeup_->signal();
    }
  }

  void subscribe(const detail::BindingToken& token, const TrackDescriptor&,
                 std::shared_ptr<detail::QueueBudget>) override {
    events_.push_back({detail::TransportEventKind::subscribed, token, "remote"});
    wakeup_->signal();
  }

  void remove(const detail::BindingToken&, bool, Removed completed = {}) override {
    if (completed) {
      completed({});
    }
  }

  detail::SendResult send(const detail::BindingToken&, const EncodedFrame&) override {
    return detail::SendResult::sent;
  }

  void request_keyframe(const detail::BindingToken&) override {}

  void stop() noexcept override {}

 private:
  std::vector<detail::TransportEvent> events_;
};

void synchronous_engine() {
  RtcEngine engine([] { return std::make_unique<LocalTransport>(); });
  RoomOptions options;
  options.operation_timeout = 100ms;
  require_result(engine.join(options));
  auto descriptor = rtc_audio_test::descriptor("active");
  check(require_result(engine.publish(descriptor)) == descriptor.id, "Graph identity changed");
  auto pending = engine.publish(rtc_audio_test::descriptor("pending"));
  check(!pending && pending.error().code == livekit::sdk::ErrorCode::timeout,
        "Missing publication timeout");
  require_result(engine.close());
  check(!engine.publish(descriptor), "Closed engine accepted publication");
}

void independent_builder_contract() {
  RoomEngine room([] { return std::make_unique<LocalTransport>(); });
  require_result(room.join({}));
  auto descriptor = rtc_audio_test::descriptor("builder");
  auto endpoint = require_result(room.publish(descriptor));
  auto receiver = require_result(room.subscribe(descriptor));
  RtcMediaResources resources({});
  PreparedMediaGraph publication, subscription;
  {
    RtcMediaGraphBuilder builder(resources);
    MediaPublishSpec spec;
    spec.track = descriptor;
    spec.input.generated = true;
    publication = builder.build_publish(
        spec, std::make_shared<nodes::RtcPublishNode>("builder.sink", endpoint));
    MediaSubscribeSpec receive;
    receive.track = descriptor;
    subscription = builder.build_subscribe(
        receive, std::make_shared<nodes::RtcSubscribeNode>("builder.source", receiver));
  }
  check(publication.track && subscription.track && !publication.graph.nodes.empty() &&
            !subscription.graph.nodes.empty(),
        "Builder did not return independent prepared graphs");
  const auto snapshot = publication.track->snapshot();
  check(snapshot.input && snapshot.input->emitted == 0,
        "Builder started media before graph installation");
  publication.graph.quiesce();
  subscription.graph.quiesce();
  check(endpoint->snapshot().state == TrackState::ready &&
            receiver->snapshot().state == TrackState::ready,
        "Builder quiescence detached a transport endpoint");
  require_result(endpoint->close());
  require_result(receiver->close());
  require_result(room.leave());
}

void shared_storage_and_clock() {
  detail::RawFrameQueue fast({2, 4096, 1s}), slow({1, 4096, 1s});
  fast.start();
  slow.start();
  auto pcm =
      std::make_shared<const PcmAudioFrame>(*rtc_audio_test::tone(1, 0, 48000, 1, false).pcm);
  const auto now = SteadyClock::now();
  check(fast.write({pcm, now, 960, 0, 123456}) && slow.write({pcm, now, 960, 0, 123456}),
        "Shared queues rejected input");
  const auto a = fast.read({}), b = slow.read({});
  check(a && b && std::get<std::shared_ptr<const PcmAudioFrame>>(a->payload).get() == pcm.get() &&
            std::get<std::shared_ptr<const PcmAudioFrame>>(b->payload).get() == pcm.get(),
        "Shared capture copied PCM per consumer");
  check(a->timestamp == 123456 && b->timestamp == 123456,
        "Shared capture replaced the sample clock with frame arrival time");
  fast.stop();
  check(slow.write({pcm, now, 960}), "Stopping one shared branch stopped another");
  slow.stop();
  check(!slow.read({}), "Stopped shared queue retained frames");

  livekit::sdk::media::FrameTiming timing;
  timing.timestamp = 480;
  timing.clock_rate = 48000;
  timing.epoch = 3;
  auto view = frame_view(pcm, timing);
  check(view.planes[0].data() == reinterpret_cast<const std::byte*>(pcm->samples.data()),
        "RTC common frame view copied samples");
  livekit::sdk::media::AudioFrame playback;
  playback.pcm = std::make_shared<const std::vector<float>>(960, 0.25f);
  playback.timestamp_us = 10000;
  playback.generation = 5;
  auto file_view = livekit::sdk::media::frame_view(playback);
  playback.pcm.reset();
  check(file_view.owner && file_view.timing.clock_rate == 1000000 && file_view.timing.epoch == 5 &&
            file_view.planes[0].size() == 3840,
        "Player view lost ownership or time base");

  detail::MediaSyncClock clock(80ms);
  timing.sender_capture_time_us = 1000000;
  clock.map(timing);
  auto video = timing;
  video.timestamp = 12345;
  video.clock_rate = 90000;
  video.sender_capture_time_us = 1033000;
  clock.map(video);
  check(video.presentation_time && timing.presentation_time &&
            *video.presentation_time - *timing.presentation_time == 33ms,
        "A/V mapping used unrelated RTP origins instead of sender time");
  auto unknown = timing;
  unknown.sender_capture_time_us.reset();
  unknown.presentation_time.reset();
  clock.map(unknown);
  check(!unknown.presentation_time, "Missing sender clock was falsely synchronized");
  const auto audio_due = SteadyClock::now() + 100ms;
  clock.observe_audio(1100000, audio_due, 3);
  video.sender_capture_time_us = 1133000;
  clock.map(video);
  check(video.presentation_time == audio_due + 33ms, "Video ignored audio-device clock drift");
  video.epoch = 2;
  clock.map(video);
  check(!video.presentation_time, "Old epoch used a new presentation clock");
}

class Source final : public graphflow::core::GraphNode {
 public:
  Source() : GraphNode("source", nullptr) {}

  void raw(nodes::AudioFrameEvent frame) {
    publish(frame);
  }

  void packet(EncodedFrame frame) {
    publish(nodes::EncodedFrameEvent(std::move(frame)));
  }

  void feedback() {
    SenderFeedback feedback;
    feedback.epoch = 1;
    feedback.revision = 1;
    feedback.encodings[""] = {64000, true, false, 0};
    publish(nodes::SenderFeedbackEvent(feedback));
  }
};

void callback_isolation() {
  std::mutex mutex;
  std::condition_variable changed;
  bool release = false;
  std::atomic<bool> entered = false;
  MediaOutput options;
  options.queue = {2, 4096, 1s};
  options.audio = [&](const auto&) {
    entered = true;
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return release; });
  };
  auto source = std::make_shared<Source>();
  auto output = std::make_shared<ApplicationOutput>(options);
  graphflow::engine::GraphEngine graph{1};
  check(graph.loadFromNodes({output, source},
                            {TrackGraphSpec::connect<nodes::AudioFrameEvent>(source, output)}) &&
            graph.init() && graph.start(),
        "Unable to start callback graph");

  struct Unblock {
    std::mutex& mutex;
    std::condition_variable& changed;
    bool& release;

    ~Unblock() {
      std::lock_guard lock(mutex);
      release = true;
      changed.notify_all();
    }
  } unblock{mutex, changed, release};

  source->raw(rtc_audio_test::tone(1, 0, 48000, 1, false));
  eventually([&] { return entered.load(); });
  for (int i = 1; i < 20; ++i) {
    source->raw(rtc_audio_test::tone(1, i * 480, 48000, 1, false));
  }
  check(output->dropped >= 16, "Slow callback did not retain a bounded backlog");
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
  output->quiesce();
  graph.stop();
  graph.release();
}

class FakeSpeaker final : public detail::AudioOutputDriver {
 public:
  Render render;
  Failure fail;

  void start(const AudioOutputOptions&, Render callback, Failure failure) override {
    render = std::move(callback);
    fail = std::move(failure);
  }

  void stop() noexcept override {
    render = {};
    fail = {};
  }
};

void active_device_failure() {
  auto driver = std::make_unique<FakeSpeaker>();
  auto* speaker = driver.get();
  auto session = detail::AudioOutputFactory::create({}, std::move(driver));
  auto first = std::make_shared<nodes::AudioOutputNode>("speaker.first", "first", session);
  auto second = std::make_shared<nodes::AudioOutputNode>("speaker.second", "second", session);
  std::promise<std::exception_ptr> a, b;
  auto ra = a.get_future(), rb = b.get_future();
  first->setFailureHandler([&](auto error) { a.set_value(error); });
  second->setFailureHandler([&](auto error) { b.set_value(error); });
  check(first->init() && second->init() && first->start() && second->start(),
        "Unable to start shared device sinks");
  // No PCM is delivered: idle sinks must still learn of device failure.
  speaker->fail("device unplugged");
  check(
      ra.wait_for(1s) == std::future_status::ready && rb.wait_for(1s) == std::future_status::ready,
      "Idle shared sinks required a health scan to observe device failure");
  const auto fa = livekit::sdk::describe_failure(ra.get());
  const auto fb = livekit::sdk::describe_failure(rb.get());
  check(fa->id == fb->id && fa->id == session->snapshot().failure->id,
        "Shared device failure lost correlation between sinks");
  first->quiesce();
  second->quiesce();
}

void retained_failure_reporter() {
  graphflow::core::GraphNode::FailureHandler report;
  std::promise<std::exception_ptr> result;
  auto ready = result.get_future();
  {
    auto node = std::make_unique<graphflow::core::GraphNode>("external.device", nullptr);
    report = node->failureReporter();
    node->setFailureHandler([&](auto error) { result.set_value(error); });
  }
  const auto cause = std::make_exception_ptr(std::runtime_error("late device callback"));
  report(cause);
  report(cause);
  check(ready.wait_for(1s) == std::future_status::ready && ready.get() == cause,
        "Retained reporter required a live node or delivered the failure twice");
}

void active_output_failure() {
  MediaOutput options;
  options.audio = [](const auto&) { throw std::runtime_error("consumer failed"); };
  auto source = std::make_shared<Source>();
  auto output = std::make_shared<ApplicationOutput>(options);
  std::promise<std::exception_ptr> result;
  auto ready = result.get_future();
  output->setFailureHandler([&](auto error) { result.set_value(error); });
  graphflow::engine::GraphEngine graph{1};
  check(graph.loadFromNodes({output, source},
                            {TrackGraphSpec::connect<nodes::AudioFrameEvent>(source, output)}) &&
            graph.init() && graph.start(),
        "Unable to start failing output graph");
  source->raw(rtc_audio_test::tone(1, 0, 48000, 1, false));
  check(ready.wait_for(1s) == std::future_status::ready,
        "Application worker did not actively report its exception");
  const auto info = livekit::sdk::describe_failure(ready.get());
  check(
      info->code == livekit::sdk::ErrorCode::callback_failed && info->message == "consumer failed",
      "Output failure lost its type or message");
  output->quiesce();
  graph.stop();
  graph.release();
}

void capture_processing_reference() {
  auto reference = std::make_shared<AudioRenderReference>();
  auto driver = std::make_unique<FakeSpeaker>();
  auto* speaker = driver.get();
  AudioOutputOptions output_options;
  output_options.buffer = 0ms;
  output_options.echo_reference = reference;
  auto output = detail::AudioOutputFactory::create(output_options, std::move(driver));
  auto track = output->open_track("speaker");
  auto tone = rtc_audio_test::tone(1, 0, 48000, 1, false);
  track->write(tone);
  std::array<int16_t, 960> played;
  speaker->render(played);
  uint64_t cursor = 0;
  auto history = reference->read_since(cursor);
  check(history.size() == 1 && history.front().stereo == played,
        "Echo reference differs from the actual mixed speaker signal");
  detail::AudioProcessor processor(AudioProcessingOptions{}, reference);
  for (int i = 0; i < 30; ++i) {
    auto input = rtc_audio_test::tone(i < 15 ? 1 : 2, i * 480, 44100, 1, true);
    processor.process(input);
    check(input.pcm->sample_rate == 48000 && input.pcm->samples.size() == 480,
          "APM lost 10ms format across resampling/epoch reset");
  }
  track->close();
  output.reset();
  auto after_close = rtc_audio_test::tone(3, 0, 48000, 1, false);
  processor.process(after_close);
  check(after_close.pcm->samples.size() == 480, "APM retained a destroyed speaker resource");
}

void echo_convergence() {
  auto reference = std::make_shared<AudioRenderReference>();
  AudioProcessingOptions options;
  options.noise_suppression = false;
  options.automatic_gain = false;
  options.device_delay = 40ms;
  detail::AudioProcessor processor(options, reference);
  std::array<std::array<int16_t, 480>, 5> delay{};
  uint32_t random = 0x12345678;
  double input_energy = 0, output_energy = 0;
  for (int block = 0; block < 600; ++block) {
    auto& current = delay[size_t(block) % delay.size()];
    std::array<int16_t, 960> render;
    for (size_t i = 0; i < 480; ++i) {
      random = random * 1664525u + 1013904223u;
      current[i] = int16_t((int(random >> 16) - 32768) / 4);
      render[i * 2] = render[i * 2 + 1] = current[i];
    }
    reference->write(render);
    auto pcm = std::make_shared<PcmAudioFrame>();
    pcm->samples.resize(480);
    const auto& echo = delay[(size_t(block) + 1) % delay.size()];
    for (size_t i = 0; i < 480; ++i) {
      pcm->samples[i] = int16_t(echo[i] * 0.6);
    }
    nodes::AudioFrameEvent frame;
    frame.pcm = pcm;
    frame.epoch = 1;
    frame.timestamp = uint64_t(block) * 480;
    processor.process(frame);
    if (block > 500) {
      for (size_t i = 0; i < 480; ++i) {
        input_energy += double(pcm->samples[i]) * pcm->samples[i];
        output_energy += double(frame.pcm->samples[i]) * frame.pcm->samples[i];
      }
    }
  }
  check(input_energy > 0 && output_energy < input_energy * 0.3,
        "AEC did not converge on a deterministic delayed echo path");
}
}  // namespace

int main() {
  try {
    synchronous_engine();
    independent_builder_contract();
    shared_storage_and_clock();
    callback_isolation();
    active_output_failure();
    active_device_failure();
    retained_failure_reporter();
    capture_processing_reference();
    echo_convergence();
    std::cout << "architecture contracts passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
