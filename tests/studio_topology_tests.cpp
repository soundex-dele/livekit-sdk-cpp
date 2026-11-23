#include "engines/studio/studio_graph.hpp"
#include "media_nodes/capture/audio_source.hpp"
#include "media_nodes/capture/video_source.hpp"
#include <windows.h>
#include <atomic>
#include <future>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <tuple>
#include <thread>

namespace {
using namespace livekit::sdk;
using Edge = std::tuple<std::string, std::string, std::string>;
using Counts = std::pair<uint64_t, uint64_t>;  // input callbacks, output publications

void require(bool ok, const char* message) {
  if (!ok) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void check_counts(const studio_detail::StudioGraph& graph,
                  const std::map<std::string, Counts>& wanted, const char* message) {
  std::map<std::string, Counts> observed;
  for (const auto& n : graph.nodes()) {
    require(observed.emplace(n.name, Counts{n.received, n.published}).second, "duplicate node");
  }
  if (observed != wanted) {
    for (const auto& [name, counts] : observed) {
      std::cerr << name << " received=" << counts.first << " published=" << counts.second << '\n';
    }
    require(false, message);
  }
}

// Exercise the startup/burst margin without depending on Windows wake-up timing.
void check_program_queue() {
  studio_detail::StudioGraphState state(StudioOptions{});
  for (const auto& source : {state.programAudio, state.localAudio}) {
    detail::PcmSourceReader reader(source);
    require(bool(reader.start()), "attach PCM reader");
    PcmInputFrame frame;
    frame.float_samples.assign(960, .25f);
    const auto origin = std::chrono::steady_clock::now() - std::chrono::milliseconds(120);
    for (int slot = 0; slot <= 12; ++slot) {
      frame.capture_time = origin + std::chrono::milliseconds(10 * slot);
      auto result = source->write(frame);
      require(result && *result == (slot < 12 ? AudioWriteStatus::accepted
                                              : AudioWriteStatus::backpressure),
              "Studio must absorb a bounded 120 ms PCM burst");
    }
    for (int slot = 0; slot < 12; ++slot) {
      auto block = reader.read();
      require(block && !block->silence &&
                  block->captured == origin + std::chrono::milliseconds(10 * slot),
              "buffered PCM slot was lost or retimed");
    }
    reader.stop();
  }
}

void run(bool fail_mix) {
  StudioOptions options;
  options.software_gpu = true;
  studio_detail::StudioGraphState state(options);
  if (fail_mix) {
    state.settings.microphone_volume = 5;  // Invalid gain, independent of any capture device.
  }
  StudioSource source;
  source.id = "white";
  source.color = {1, 1, 1, 1};
  state.scene.sources.push_back(source);
  // A delayed first tick can catch up several 10-ms slots. Mark every one.
  auto& samples = state.remoteAudio["marker"];
  for (int i = 0; i < 9600; ++i) {
    samples.push_back(.5f);
    samples.push_back(-.5f);
  }
  detail::I420SourceReader video(state.programVideo);
  detail::PcmSourceReader audio(state.programAudio);
  require(video.start() && audio.start(), "attach program readers");
  // Hold the actual video owner inside step(). Only a distinct clock can advance PCM here.
  std::atomic<bool> holding{false};
  std::promise<void> progressed;
  auto progress = progressed.get_future();
  int during_hold = 0;
  std::chrono::steady_clock::time_point last{};
  state.traceProgramWrite = [&](auto slot, auto, auto, auto, auto admission) {
    require(last == decltype(last){} || slot - last == std::chrono::milliseconds(10),
            "audio slot gap");
    last = slot;
    if (holding.load() && admission == AudioWriteStatus::accepted && ++during_hold == 8)
      progressed.set_value();
  };
  state.traceVideoDispatch = [&](auto, auto) {
    holding = true;
    unsigned markers = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
      auto block = audio.read();
      if (block && !block->silence && block->samples[48] > 15000 && block->samples[49] < -15000)
        ++markers;
      require(std::chrono::steady_clock::now() < until, "video stalled the program audio clock");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (progress.wait_for(std::chrono::seconds(0)) != std::future_status::ready ||
             markers < 8);
    holding = false;
  };
  studio_detail::StudioGraph graph(state);
  auto started = graph.start();
  require(bool(started) != fail_mix, "unexpected first audio slot result");
  const std::multiset<Edge> expected = {
      {"studio.clock", "studio.recorder", "Tick"},
      {"studio.clock", "studio.sources", "Tick"},
      {"studio.audio-clock", "studio.audio-sources", "AudioTick"},
      {"studio.audio-sources", "studio.mixer", "AudioInput"},
      {"studio.sources", "studio.compositor", "VideoLayers"},
      {"studio.mixer", "studio.audio-output", "MixedAudio"},
      {"studio.mixer", "studio.monitor", "MixedAudio"},
      {"studio.mixer", "studio.audio-recorder", "RecordingAudio"},
      {"studio.compositor", "studio.video-output", "ComposedVideo"},
      {"studio.compositor", "studio.preview", "ComposedVideo"},
      {"studio.compositor", "studio.recorder", "RecordingVideo"},
  };
  std::multiset<Edge> actual;
  for (const auto& edge : graph.connections()) {
    actual.emplace(edge.from, edge.to, edge.type);
  }
  require(actual == expected, "missing, duplicated or mislabelled Studio edge");

  if (fail_mix) {
    require(!started && started.failure_record(), "mixer should reject invalid gain");
    require(graph.start().failure_record() == started.failure_record(), "lost first slot failure");
    const std::map<std::string, Counts> prefix = {
        {"studio.clock", {0, 1}},          {"studio.sources", {0, 0}},
        {"studio.audio-clock", {0, 1}},    {"studio.audio-sources", {1, 1}},
        {"studio.mixer", {1, 0}},          {"studio.recorder", {1, 0}},
        {"studio.audio-recorder", {0, 0}}, {"studio.compositor", {0, 0}},
        {"studio.preview", {0, 0}},        {"studio.audio-output", {0, 0}},
        {"studio.video-output", {0, 0}},   {"studio.monitor", {0, 0}}};
    check_counts(graph, prefix, "tick failure lost control prefix or ran downstream callbacks");
    require(!state.status.preview && state.status.frames == 0 &&
                state.programAudio->stats().accepted_samples == 0 &&
                state.programVideo->stats().accepted == 0,
            "unexpected program output on failed tick");
    video.stop();
    audio.stop();
    graph.release();
    std::cout << "failed tick: recorder control before mixer failure, no terminal media\n";
    return;
  }
  auto block = audio.read();
  require(block && !block->silence && block->samples.size() == 960 && block->samples[48] > 15000 &&
              block->samples[49] < -15000,
          "program PCM missing stereo marker");
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard lock(state.mutex);
    state.remoteSeen["expired"] = now - std::chrono::seconds(5);
    state.remoteSeen["marker"] = now;
    state.remoteAudio["expired"].push_back(1.f);
    state.remoteVideo["expired"] = {};
  }
  // A publisher consumes concurrently. Leaving the only frame in the source until the
  // deliberate video-owner stall finishes can expire it; no second step can unblock read().
  auto firstVideo = std::async(std::launch::async, [&] { return video.read({}); });
  auto tick = graph.step(now);
  {
    std::lock_guard lock(state.mutex);
    require(!state.remoteSeen.count("expired") && !state.remoteAudio.count("expired") &&
                !state.remoteVideo.count("expired") && state.remoteSeen.count("marker"),
            "expired feeds retained");
  }
  require(bool(tick), "tick");
  require(firstVideo.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "timed out consuming the single program video frame");
  graph.stop();
  // Delayed inspection is valid for a consumed frame, not for an expired source queue entry.
  std::this_thread::sleep_for(state.programVideo->options().max_age + std::chrono::milliseconds(20));
  auto frame = firstVideo.get();
  require(frame && frame->frame.width == options.width && frame->frame.height == options.height &&
              !frame->frame.y.empty() && frame->frame.y.front() > 200,
          "program I420 missing white marker");
  const auto as = state.programAudio->stats();
  require(as.accepted_samples && as.accepted_samples % 480 == 0, "incomplete program audio slot");
  const uint64_t slots = as.accepted_samples / 480 + as.rejected_writes;
  require(
      state.status.preview && state.status.frames == 1 && state.programVideo->stats().accepted == 1,
      "preview/program disagree on frame count");
  const std::map<std::string, Counts> wanted = {
      {"studio.clock", {0, 2}},
      {"studio.sources", {1, 1}},
      {"studio.audio-clock", {0, slots}},
      {"studio.audio-sources", {slots, slots}},
      {"studio.audio-recorder", {slots, 0}},
      {"studio.mixer", {slots, slots * 2}},
      {"studio.compositor", {1, 2}},
      {"studio.audio-output", {slots, 0}},
      {"studio.video-output", {1, 0}},
      {"studio.monitor", {slots, 0}},
      {"studio.preview", {1, 0}},
      {"studio.recorder", {3, 0}},  // startup control, step control and a video frame
  };
  check_counts(graph, wanted, "Studio graph counters disagree with the terminal deliveries");
  video.stop();
  audio.stop();
  graph.release();
  std::cout << actual.size() << " edges, " << wanted.size() << " nodes, " << slots
            << " audio slot(s), one marked video frame\n";
}
}  // namespace

int main() {
  require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "initialize COM");
  check_program_queue();
  run(false);
  run(true);
  CoUninitialize();
}
