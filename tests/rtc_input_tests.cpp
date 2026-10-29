#include "livekit/rtc/input_node.hpp"
#include "livekit/rtc/stream_pipeline.hpp"
#include "media_nodes/capture/microphone_driver.hpp"
#include "media_nodes/capture/microphone_transport.hpp"
#include "media_nodes/capture/audio_source.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <condition_variable>
#include <cmath>
#include <numbers>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

namespace {
void check(bool b, const char* message) {
  if (!b)
    throw std::runtime_error(message);
}

template <class F>
void eventually(F f, const char* message) {
  const auto end = SteadyClock::now() + 4s;
  while (!f()) {
    check(SteadyClock::now() < end, message);
    std::this_thread::sleep_for(1ms);
  }
}

TrackDescriptor video() {
  TrackDescriptor d;
  d.id = "video";
  d.format.codec = Codec::vp8;
  d.format.width = 64;
  d.format.height = 48;
  return d;
}

TrackDescriptor audio() {
  auto d = video();
  d.id = "audio";
  d.kind = MediaKind::audio;
  d.format = {};
  d.format.codec = Codec::opus;
  d.format.clock_rate = 48000;
  d.format.channels = 2;
  return d;
}

class Feedback : public graphflow::core::GraphNode {
 public:
  Feedback() : GraphNode("feedback", nullptr) {}

  void send(Epoch epoch, uint64_t revision = 1, uint64_t generation = 0, bool key = false,
            bool active = true) {
    SenderFeedback value;
    value.epoch = epoch;
    value.revision = revision;
    value.media_generation = generation;
    value.encodings[""] = {64000, active, key, key ? revision : 0};
    publish(nodes::SenderFeedbackEvent(value));
  }
};

class Output : public graphflow::core::GraphNode {
 public:
  Output() : GraphNode("output", nullptr) {}

  std::mutex mutex;
  std::condition_variable changed;
  bool block = false, entered = false;
  std::vector<nodes::VideoFrameEvent> videos;
  std::vector<nodes::AudioFrameEvent> audios;
  std::vector<EncodedFrame> packets;

  void unblock() {
    std::lock_guard lock(mutex);
    block = false;
    changed.notify_all();
  }

 protected:
  bool doInit() override {
    subscribe<nodes::VideoFrameEvent>([this](const auto& event) {
      std::unique_lock lock(mutex);
      videos.push_back(event);
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return !block; });
    });
    subscribe<nodes::AudioFrameEvent>([this](const auto& event) {
      std::lock_guard lock(mutex);
      audios.push_back(event);
    });
    subscribe<nodes::EncodedFrameEvent>([this](const auto& event) {
      std::lock_guard lock(mutex);
      packets.push_back(event.frame);
    });
    return true;
  }
};

struct Graph {
  std::shared_ptr<Feedback> feedback = std::make_shared<Feedback>();
  std::shared_ptr<Output> output = std::make_shared<Output>();
  std::shared_ptr<nodes::MediaInputNode> input;
  graphflow::engine::GraphEngine graph{1};

  Graph(TrackDescriptor d, nodes::MediaInputOptions options) {
    input = std::make_shared<nodes::MediaInputNode>("input", d, std::move(options));
    using G = engine::TrackGraphSpec;
    check(graph.loadFromNodes({output, input, feedback},
                              {G::connect<nodes::SenderFeedbackEvent>(feedback, input),
                               G::connect<nodes::VideoFrameEvent>(input, output),
                               G::connect<nodes::AudioFrameEvent>(input, output),
                               G::connect<nodes::EncodedFrameEvent>(input, output)}) &&
              graph.init() && graph.start(),
          "Input graph start failed");
  }

  ~Graph() {
    output->unblock();
    input->quiesce();
    graph.stop();
    graph.release();
  }
};

class FakeMicrophone final : public livekit::sdk::detail::MicrophoneDriver {
 public:
  std::mutex mutex;
  Callback callback;
  std::atomic<int> starts{0}, stops{0};
  bool fail_start = false;

  void start(const livekit::sdk::MicrophoneOptions&, Callback value) override {
    std::lock_guard lock(mutex);
    callback = std::move(value);
    ++starts;
    if (fail_start)
      throw std::runtime_error("Injected microphone startup failure");
  }

  void stop() noexcept override {
    std::lock_guard lock(mutex);
    if (callback)
      ++stops;
    callback = {};
  }

  void push(const std::vector<int16_t>& samples, int rate = 48000, size_t channels = 1,
            SteadyClock::time_point captured = SteadyClock::now()) {
    std::lock_guard lock(mutex);
    if (callback)
      callback({samples, rate, channels, captured});
  }
};

void microphone_input() {
  auto device = std::make_unique<FakeMicrophone>();
  auto* driver = device.get();
  livekit::sdk::MicrophoneOptions config;
  config.channels = 2;
  auto microphone = livekit::sdk::detail::MicrophoneFactory::create(config, std::move(device));
  nodes::MediaInputOptions options;
  options.microphone = microphone;
  Graph first(audio(), options);
  bool duplicate = false;
  try {
    Graph second(audio(), options);
  } catch (const std::invalid_argument&) {
    duplicate = true;
  }
  check(duplicate, "One microphone admitted two independent source readers");
  first.input->activate();
  std::this_thread::sleep_for(20ms);
  check(driver->starts == 0, "Microphone opened before binding feedback");
  first.feedback->send(1);
  eventually([&] { return driver->starts == 1; }, "Microphone did not start with input graph");
  driver->push(std::vector<int16_t>(160, 5000), 16000);
  eventually(
      [&] {
        std::lock_guard lock(first.output->mutex);
        return !first.output->audios.empty();
      },
      "Resampled microphone PCM missing");
  {
    std::lock_guard lock(first.output->mutex);
    const auto& frame = *first.output->audios.front().pcm;
    check(frame.sample_rate == 48000 && frame.channels == 2 && frame.samples.size() == 960,
          "Microphone format conversion failed");
    int peak = 0;
    for (size_t i = 0; i < frame.samples.size(); i += 2) {
      check(frame.samples[i] == frame.samples[i + 1],
            "Microphone mono duplication changed channel routing");
      peak = std::max(peak, std::abs(int(frame.samples[i])));
    }
    check(peak > 1000, "Microphone resampler produced silence");
  }
  first.feedback->send(2);
  driver->push(std::vector<int16_t>(480, 7000));
  eventually(
      [&] {
        std::lock_guard lock(first.output->mutex);
        for (const auto& f : first.output->audios)
          if (f.epoch == 2 && f.pcm->samples.back() == 7000)
            return true;
        return false;
      },
      "Microphone lost first frame after epoch reset");
  check(driver->starts == 1, "Room epoch change restarted physical microphone");
  first.input->quiesce();
  check(driver->stops == 1 && !microphone->snapshot().running &&
            !microphone->source()->stats().attached,
        "Microphone quiescence retained callbacks or source lease");
  const auto captured = microphone->snapshot().captured_blocks;
  driver->push(std::vector<int16_t>(480, 99));
  check(microphone->snapshot().captured_blocks == captured,
        "Microphone callback survived quiescence");
  Graph replacement(audio(), options);
  replacement.feedback->send(3);
  replacement.input->activate();
  eventually([&] { return driver->starts == 2; },
             "Retained microphone could not restart in replacement graph");
  driver->push(std::vector<int16_t>(480, 3000));
  eventually(
      [&] {
        std::lock_guard lock(replacement.output->mutex);
        return !replacement.output->audios.empty();
      },
      "Restarted microphone did not emit");
  driver->push(std::vector<int16_t>(13, 1));
  eventually([&] { return replacement.input->snapshot().finished; },
             "Malformed microphone callback did not wake input worker");
  check(!replacement.input->snapshot().error.empty() && driver->stops == 2,
        "Microphone failure was hidden or device remained running");
}

void microphone_queue() {
  auto device = std::make_unique<FakeMicrophone>();
  auto* driver = device.get();
  livekit::sdk::MicrophoneOptions options;
  options.max_queue_duration = 10ms;
  auto microphone = livekit::sdk::detail::MicrophoneFactory::create(options, std::move(device));
  livekit::sdk::detail::PcmSourceReader reader(microphone->source());
  reader.start();
  driver->fail_start = true;
  bool failed = false;
  try {
    microphone->start();
  } catch (const std::runtime_error&) {
    failed = true;
  }
  check(failed && !microphone->snapshot().running && driver->stops == 1,
        "Partial microphone startup did not release callbacks");
  driver->fail_start = false;
  microphone->start();
  const auto now = SteadyClock::now();
  std::vector<int16_t> stereo(960);
  for (size_t i = 0; i < 480; ++i) {
    stereo[i * 2] = 12000;
    stereo[i * 2 + 1] = -4000;
  }
  driver->push(stereo, 48000, 2, now);
  driver->push(stereo, 48000, 2, now + 10ms);
  check(microphone->snapshot().captured_blocks == 2 && microphone->snapshot().dropped_blocks == 1 &&
            microphone->source()->stats().queued_blocks == 1,
        "Device capture did not respect bounded queue");
  auto mixed = reader.read({});
  check(mixed && mixed->samples.size() == 480 && mixed->samples.front() == 4000,
        "Microphone stereo downmix overflowed or routed incorrectly");
  microphone->stop();
  reader.stop();
  // A source generation change resets capture cadence; the first block is fresh.
  reader.start();
  microphone->start();
  driver->push(std::vector<int16_t>(480, 8000));
  auto fresh = reader.read({});
  check(fresh && fresh->samples.front() == 8000, "Restart kept stale microphone PCM");
  microphone->stop();
  microphone->start();
  driver->push(std::vector<int16_t>(480, 9000));
  auto resumed = reader.read({});
  check(resumed && resumed->samples.front() == 9000 && microphone->snapshot().error.empty(),
        "Device restart within one reader generation overlapped PCM time");
  microphone->stop();
  reader.stop();
  const auto retired = microphone->source()->stats().generation;
  reader.start();
  livekit::sdk::PcmInputFrame late;
  late.samples.assign(480, 19000);
  late.capture_time = SteadyClock::now() + 20ms;
  check(livekit::sdk::detail::PcmSourceWriter::write(*microphone->source(), late, retired) ==
                livekit::sdk::AudioWriteStatus::backpressure &&
            microphone->source()->stats().queued_blocks == 0,
        "In-flight device write crossed source generation despite future timestamp");
  reader.stop();
}

void microphone_callback() {
  std::vector<int16_t> captured;
  size_t captured_channels = 0;
  int captured_rate = 0;
  SteadyClock::time_point when;
  auto transport = livekit::sdk::detail::create_microphone_transport([&](const auto& block) {
    captured.assign(block.samples.begin(), block.samples.end());
    captured_channels = block.channels;
    captured_rate = block.sample_rate;
    when = block.captured;
  });
  std::vector<int16_t> input(960, 3000);
  uint32_t level = 99;
  const auto before = SteadyClock::now();
  check(transport->RecordedDataIsAvailable(input.data(), 480, 4, 2, 48000, 25, 0, 17, false,
                                           level) == 0 &&
            level == 17 && captured == input && captured_channels == 2 && captured_rate == 48000,
        "Native microphone callback misread interleaved byte count");
  check(when >= before - 25ms && when <= SteadyClock::now() - 25ms,
        "Native microphone callback did not account for capture delay");
  transport->RecordedDataIsAvailable(input.data(), 480, 2, 2, 48000, 0, 0, 17, false, level);
  check(captured.empty(), "Malformed microphone format was exposed as valid PCM");
  size_t out = 0;
  int64_t elapsed = 99, ntp = 99;
  transport->NeedMorePlayData(480, 4, 2, 48000, input.data(), out, &elapsed, &ntp);
  check(out == 960 && elapsed == -1 && ntp == -1 &&
            std::all_of(input.begin(), input.end(), [](auto v) { return v == 0; }),
        "Unused microphone render callback left uninitialized output");
}

void generated_demand() {
  auto descriptor = video();
  descriptor.follow_subscriber_demand = true;
  nodes::MediaInputOptions options;
  options.generated = true;
  Graph graph(descriptor, options);
  graph.feedback->send(1, 1, 0, false, false);
  graph.input->activate();
  std::this_thread::sleep_for(100ms);
  check(graph.input->snapshot().emitted == 0, "Generated source ran while initial demand was OFF");
  graph.feedback->send(1, 2, 0, true, true);
  eventually([&] { return graph.input->snapshot().emitted >= 3; },
             "Generated source did not resume");
  graph.feedback->send(1, 3, 0, false, false);
  // Allow already-admitted delivery to finish before taking the stable baseline.
  std::this_thread::sleep_for(50ms);
  const auto paused = graph.input->snapshot().emitted;
  std::this_thread::sleep_for(120ms);
  check(graph.input->snapshot().emitted == paused,
        "Generated source kept producing while demand was OFF");
  graph.feedback->send(1, 4, 0, true, true);
  eventually([&] { return graph.input->snapshot().emitted >= paused + 3; },
             "Generated source did not restart with same epoch");
  graph.feedback->send(1, 5, 0, false, false);
  graph.input->quiesce();
  graph.input->throw_if_failed();
}

void generated() {
  nodes::MediaInputOptions options;
  options.generated = true;
  Graph a(audio(), options), v(video(), options);
  a.input->activate();
  v.input->activate();
  std::this_thread::sleep_for(20ms);
  check(a.input->snapshot().emitted == 0 && v.input->snapshot().emitted == 0,
        "Input produced before binding feedback");
  a.feedback->send(1);
  v.feedback->send(1);
  eventually(
      [&] {
        std::lock_guard lock(a.output->mutex);
        return a.output->audios.size() >= 5;
      },
      "Generated PCM missing");
  eventually(
      [&] {
        std::lock_guard lock(v.output->mutex);
        return v.output->videos.size() >= 3;
      },
      "Generated pixels missing");
  a.input->quiesce();
  const auto emitted = a.input->snapshot().emitted;
  eventually(
      [&] {
        std::lock_guard lock(v.output->mutex);
        return v.output->videos.size() >= 5;
      },
      "Stopping audio stopped independent video");
  check(a.input->snapshot().emitted == emitted, "Producer survived quiescence");
  {
    std::lock_guard lock(a.output->mutex);
    for (size_t i = 0; i < a.output->audios.size(); ++i) {
      const auto& event = a.output->audios[i];
      check(event.epoch == 1 && event.pcm->samples_per_channel() == 480 && event.pcm->channels == 2,
            "Generated PCM format lost");
      if (i)
        check(event.timestamp > a.output->audios[i - 1].timestamp &&
                  (event.timestamp - a.output->audios[i - 1].timestamp) % 480 == 0,
              "Generated PCM clock discontinuity");
    }
  }
  v.input->quiesce();
  a.input->throw_if_failed();
  v.input->throw_if_failed();
}

livekit::sdk::VideoFrame pixels(uint8_t luma) {
  livekit::sdk::VideoFrame f;
  f.width = 64;
  f.height = 48;
  f.stride_y = 64;
  f.stride_u = f.stride_v = 32;
  f.y.assign(64 * 48, luma);
  f.u.assign(32 * 24, 128);
  f.v = f.u;
  return f;
}

void application_video() {
  livekit::sdk::I420SourceOptions source_options;
  source_options.width = 64;
  source_options.height = 48;
  auto source = std::make_shared<livekit::sdk::I420VideoSource>(source_options);
  nodes::MediaInputOptions options;
  options.video_source = source;
  Graph graph(video(), options);
  graph.feedback->send(1);
  graph.input->activate();
  {
    std::lock_guard lock(graph.output->mutex);
    graph.output->block = true;
  }
  check(source->write(pixels(10)) == livekit::sdk::VideoWriteStatus::accepted,
        "Application pixels rejected");
  eventually(
      [&] {
        std::lock_guard lock(graph.output->mutex);
        return graph.output->entered;
      },
      "Video worker never entered consumer");
  source->write(pixels(20), SteadyClock::now() +
                                50ms);  // Even a future-stamped old generation must be flushed.
  graph.feedback->send(2);
  check(source->stats().queued_frames == 0, "Epoch retained source queue");
  check(source->write(pixels(30)) == livekit::sdk::VideoWriteStatus::accepted,
        "New epoch pixels rejected");
  graph.output->unblock();
  eventually(
      [&] {
        std::lock_guard lock(graph.output->mutex);
        return graph.output->videos.size() >= 2;
      },
      "New epoch first frame lost");
  {
    std::lock_guard lock(graph.output->mutex);
    check(graph.output->videos.back().epoch == 2 && graph.output->videos.back().pixels->y[0] == 30,
          "Old source frame retagged as current epoch");
  }
  graph.feedback->send(2, 2, 1);
  check(source->stats().queued_frames == 0, "Mute generation retained source queue");
  const auto begin = SteadyClock::now();
  graph.input->quiesce();
  check(SteadyClock::now() - begin < 1s && !source->stats().attached,
        "Quiescence did not release blocked reader/lease");
  Graph replacement(video(), options);
  replacement.feedback->send(3);
  replacement.input->activate();
  check(source->write(pixels(40)) == livekit::sdk::VideoWriteStatus::accepted,
        "Quiesced source could not be rebound");
  eventually(
      [&] {
        std::lock_guard lock(replacement.output->mutex);
        return !replacement.output->videos.empty();
      },
      "Replacement source did not deliver");
}

void application_audio() {
  livekit::sdk::PcmSourceOptions so;
  so.sample_rate = 44100;
  so.channels = 2;
  auto source = std::make_shared<livekit::sdk::PcmAudioSource>(so);
  nodes::MediaInputOptions options;
  options.audio_source = source;
  Graph graph(audio(), options);
  graph.feedback->send(1);
  graph.input->activate();
  livekit::sdk::PcmInputFrame data;
  data.samples.assign(441 * 2 * 4, 1234);
  check(source->write(data) == livekit::sdk::AudioWriteStatus::accepted,
        "Application PCM rejected");
  // A real-time reader may skip late blocks when the test host is busy. Keep
  // supplying PCM until four actual data blocks arrive; verify silence and
  // clock gaps explicitly instead of assuming four uninterrupted time slots.
  eventually(
      [&] {
        const auto status = source->write(data);
        check(status == livekit::sdk::AudioWriteStatus::accepted ||
                  status == livekit::sdk::AudioWriteStatus::backpressure,
              "Active PCM source closed");
        std::lock_guard lock(graph.output->mutex);
        return std::count_if(graph.output->audios.begin(), graph.output->audios.end(),
                             [](const auto& f) { return f.pcm->samples[0] == 1234; }) >= 4;
      },
      "Application PCM not paced");
  graph.input->quiesce();
  std::lock_guard lock(graph.output->mutex);
  for (size_t i = 0; i < graph.output->audios.size(); ++i) {
    const auto& f = graph.output->audios[i];
    check(f.pcm->sample_rate == 44100 && f.pcm->samples_per_channel() == 441,
          "Input resampled application PCM");
    const auto sample = f.pcm->samples[0];
    check(
        (sample == 1234 || sample == 0) && std::all_of(f.pcm->samples.begin(), f.pcm->samples.end(),
                                                       [&](auto value) { return value == sample; }),
        "Input corrupted application PCM");
    if (i) {
      const auto& previous = graph.output->audios[i - 1];
      const auto slots = (f.captured - previous.captured) / 10ms;
      check(slots > 0 && f.timestamp == previous.timestamp + uint64_t(slots) * 480,
            "Application PCM timestamp not 48 kHz");
    }
  }
}

void encoded_audio() {
  livekit::sdk::EncodedSourceOptions so;
  so.codec = "opus";
  auto source = std::make_shared<livekit::sdk::EncodedMediaSource>(so);
  nodes::MediaInputOptions options;
  options.encoded_source = source;
  Graph graph(audio(), options);
  graph.feedback->send(1);
  graph.input->activate();
  for (uint32_t timestamp : {0xffffff00u, 0x000002c0u}) {
    livekit::sdk::EncodedMediaFrame frame;
    frame.data = {0xf8, 0xff, 0xfe};
    frame.rtp_timestamp = timestamp;
    check(source->write(std::move(frame)) == livekit::sdk::EncodedWriteStatus::accepted,
          "Encoded source rejected packet");
  }
  eventually(
      [&] {
        std::lock_guard lock(graph.output->mutex);
        return graph.output->packets.size() == 2;
      },
      "Encoded Opus required a PCM clock");
  {
    std::lock_guard lock(graph.output->mutex);
    check(graph.output->audios.empty() && graph.output->packets[1].timestamp == 0x1000002c0ULL &&
              graph.output->packets[1].buffer.size() == 3,
          "Encoded input lost wrap or entered PCM pipeline");
  }
  livekit::sdk::EncodedMediaFrame future;
  future.data = {0xf8, 0xff, 0xfe};
  future.rtp_timestamp = 2000;
  future.captured = SteadyClock::now() + 900ms;
  source->write(std::move(future));
  std::this_thread::sleep_for(20ms);
  const auto reset = SteadyClock::now();
  graph.feedback->send(2);
  livekit::sdk::EncodedMediaFrame frame;
  frame.data = {0xf8, 0xff, 0xfe};
  frame.rtp_timestamp = 960;
  source->write(std::move(frame));
  eventually(
      [&] {
        std::lock_guard lock(graph.output->mutex);
        return graph.output->packets.size() == 3;
      },
      "New encoded epoch first packet lost");
  {
    std::lock_guard lock(graph.output->mutex);
    check(graph.output->packets.back().epoch == 2 && graph.output->packets.back().timestamp == 960,
          "Encoded epoch retained old clock");
  }
  check(SteadyClock::now() - reset < 600ms,
        "New encoded epoch waited for previous future capture time");
  graph.input->quiesce();
  graph.input->throw_if_failed();
}

void process_inputs(const std::string& executable) {
  nodes::MediaInputOptions options;
  options.ffmpeg = executable;
  options.input = "rtc-test://partial-pcm";
  Graph finite(audio(), options);
  finite.feedback->send(1);
  finite.input->activate();
  eventually([&] { return finite.input->snapshot().finished; }, "Finite input did not finish");
  finite.input->throw_if_failed();
  {
    std::lock_guard lock(finite.output->mutex);
    check(finite.output->audios.size() == 2, "Final partial PCM block missing");
    const auto& samples = finite.output->audios.back().pcm->samples;
    check(samples[0] == 1234 && samples[479] == 1234 && samples[480] == 0 && samples.back() == 0,
          "Partial PCM padding corrupted samples");
  }
  options.input = "rtc-test://blocked";
  Graph blocked(audio(), options);
  blocked.feedback->send(1);
  blocked.input->activate();
  eventually(
      [&] {
        std::lock_guard lock(blocked.output->mutex);
        return !blocked.output->audios.empty();
      },
      "Blocking fixture did not start");
  const auto start = SteadyClock::now();
  blocked.input->quiesce();
  check(SteadyClock::now() - start < 1500ms, "Stopping input did not terminate blocked child/read");
  options.input = "rtc-test://eof-blocked";
  Graph eof(audio(), options);
  eof.feedback->send(1);
  eof.input->activate();
  eventually(
      [&] {
        std::lock_guard lock(eof.output->mutex);
        return !eof.output->audios.empty();
      },
      "EOF fixture did not start");
  std::this_thread::sleep_for(70ms);
  const auto eof_start = SteadyClock::now();
  eof.input->quiesce();
  check(SteadyClock::now() - eof_start < 1500ms, "Waiting for child exit prevented cancellation");
  options.input = "rtc-test://truncated-video";
  Graph malformed(video(), options);
  malformed.feedback->send(1);
  malformed.input->activate();
  eventually([&] { return malformed.input->snapshot().finished; }, "Malformed input hung");
  check(!malformed.input->snapshot().error.empty(), "Truncated I420 accepted");
}

int fixture_child(int argc, char** argv) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::string input;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "-i")
      input = argv[i + 1];
  if (input == "rtc-test://blocked" || input == "rtc-test://eof-blocked") {
    std::vector<int16_t> samples(480 * 2, 1234);
    std::cout.write(reinterpret_cast<const char*>(samples.data()), samples.size() * 2);
    std::cout.flush();
    if (input == "rtc-test://eof-blocked")
      std::fclose(stdout);
    std::this_thread::sleep_for(20s);
    return 1;
  }
  if (input == "rtc-test://partial-pcm") {
    std::vector<int16_t> samples(480 * 2 + 240 * 2, 1234);
    std::cout.write(reinterpret_cast<const char*>(samples.data()), samples.size() * 2);
    return 0;
  }
  if (input == "rtc-test://truncated-video") {
    std::cout << "short";
    return 0;
  }
  return 2;
}

void real_ffmpeg(const std::string& ffmpeg) {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("livekit-rtc-input-" + std::to_string(SteadyClock::now().time_since_epoch().count()));
  check(std::filesystem::create_directory(directory), "Cannot create file-input fixture directory");
  const auto file = directory / "tone.wav";

  struct Cleanup {
    std::filesystem::path file, directory;

    ~Cleanup() {
      std::error_code e;
      std::filesystem::remove(file, e);
      std::filesystem::remove(directory, e);
    }
  } cleanup{file, directory};

  {
    std::ofstream out(file, std::ios::binary);
    const auto u16 = [&](uint16_t value) {
      out.put(char(value));
      out.put(char(value >> 8));
    };
    const auto u32 = [&](uint32_t value) {
      u16(uint16_t(value));
      u16(uint16_t(value >> 16));
    };
    out.write("RIFF", 4);
    u32(36 + 4410 * 2);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(44100);
    u32(88200);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(4410 * 2);
    for (int i = 0; i < 4410; ++i)
      u16(uint16_t(int16_t(6000 * std::sin(2 * std::numbers::pi * 440 * i / 44100.0))));
    check(bool(out), "Cannot write file-input fixture");
  }
  const auto utf8 = file.u8string();
  const std::string path(utf8.begin(), utf8.end());
  const auto streams = livekit::sdk::probe_media(path, ffmpeg, false, 64, 48, 30);
  check(streams.size() == 1 && !streams.front().video,
        "Standalone media probe did not discover WAV audio");
  nodes::MediaInputOptions options;
  options.ffmpeg = ffmpeg;
  options.input = path;
  Graph graph(audio(), options);
  graph.feedback->send(1);
  graph.input->activate();
  eventually([&] { return graph.input->snapshot().finished; }, "Real FFmpeg input did not finish");
  graph.input->throw_if_failed();
  std::lock_guard lock(graph.output->mutex);
  check(graph.output->audios.size() == 10, "Real FFmpeg did not resample 100 ms to ten PCM blocks");
  int peak = 0;
  for (const auto& frame : graph.output->audios) {
    check(frame.pcm->sample_rate == 48000 && frame.pcm->channels == 2,
          "FFmpeg PCM output format mismatch");
    for (size_t i = 0; i < frame.pcm->samples.size(); i += 2) {
      check(frame.pcm->samples[i] == frame.pcm->samples[i + 1],
            "Mono conversion changed channel routing");
      peak = std::max(peak, std::abs(int(frame.pcm->samples[i])));
    }
  }
  check(peak > 1000, "FFmpeg produced silence instead of fixture audio");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "-nostdin")
    return fixture_child(argc, argv);
  try {
    if (argc == 3 && std::string(argv[1]) == "--ffmpeg") {
      real_ffmpeg(argv[2]);
      std::cout << "RTC real FFmpeg input passed\n";
      return 0;
    }
    microphone_callback();
    microphone_input();
    microphone_queue();
    generated_demand();
    generated();
    application_video();
    application_audio();
    encoded_audio();
    process_inputs(std::filesystem::absolute(argv[0]).string());
    std::cout << "RTC input graph contracts passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
