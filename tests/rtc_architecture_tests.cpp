#include "livekit/rtc/executor.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "livekit/rtc/frame_view.hpp"
#include "livekit/media_core/frame.hpp"
#include "backends/rtc/room_transport.hpp"
#include "media_graph/raw_frame_queue.hpp"
#include "media_graph/sync_clock.hpp"
#include "media_nodes/capture/audio_processor.hpp"
#include "media_nodes/playout/audio_output_driver.hpp"
#include "engines/rtc/application_output.hpp"
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
  }

  std::vector<detail::TransportEvent> poll(std::chrono::milliseconds, std::stop_token) override {
    return std::exchange(events_, {});
  }

  void publish(const detail::BindingToken& token, const TrackDescriptor&) override {
    if (token.id != "pending") {
      events_.push_back({detail::TransportEventKind::published, token, "sid"});
    }
  }

  void subscribe(const detail::BindingToken& token, const TrackDescriptor&,
                 std::shared_ptr<detail::QueueBudget>) override {
    events_.push_back({detail::TransportEventKind::subscribed, token, "remote"});
  }

  std::future<void> remove(const detail::BindingToken&, bool) override {
    std::promise<void> done;
    done.set_value();
    return done.get_future();
  }

  detail::SendResult send(const detail::BindingToken&, const EncodedFrame&) override {
    return detail::SendResult::sent;
  }

  void request_keyframe(const detail::BindingToken&) override {}

  void stop() noexcept override {}

 private:
  std::vector<detail::TransportEvent> events_;
};

void automatic_executor() {
  RtcExecutor executor([] {
    return std::make_unique<RtcEngine>([] { return std::make_unique<LocalTransport>(); });
  });
  // A custom backend needs neither LiveKit credentials nor protocol options.
  executor.submit([](auto& engine) { return engine.join(RoomOptions{}); }).get();
  auto descriptor = rtc_audio_test::descriptor("active");
  const auto id =
      executor.submit([descriptor](auto& engine) { return engine.publish(descriptor); }).get();
  check(id == descriptor.id, "Automatic driver did not complete graph creation");
  check(executor.snapshot().commands_started > 0, "Executor failed to execute commands");
  executor.submit([](auto& engine) { return engine.leave(); }).get();
  executor.submit([](auto& engine) { return engine.join(RoomOptions{}); }).get();
  descriptor.id = "pending";
  auto pending = executor.submit([descriptor](auto& engine) { return engine.publish(descriptor); });
  executor.shutdown();
  check(pending.wait_for(0ms) == std::future_status::ready,
        "Shutdown left an unresolved operation");
  bool rejected = false;
  try {
    pending.get();
  } catch (...) {
    rejected = true;
  }
  check(rejected, "Shutdown unexpectedly completed an unacknowledged publication");
  auto late = executor.submit([](auto&) { return 42; });
  rejected = false;
  try {
    late.get();
  } catch (...) {
    rejected = true;
  }
  check(rejected && !executor.snapshot().running, "Stopped executor accepted work");
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

  void start(const AudioOutputOptions&, Render callback, Failure) override {
    render = std::move(callback);
  }

  void stop() noexcept override {
    render = {};
  }
};

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
    automatic_executor();
    shared_storage_and_clock();
    callback_isolation();
    capture_processing_reference();
    echo_convergence();
    std::cout << "architecture contracts passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
