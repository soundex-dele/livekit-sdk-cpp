#include "engines/studio/studio_graph.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <future>
#include <iostream>
#include <thread>

using namespace livekit::sdk;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
void require(bool ok, const char* message) {
  if (!ok) {
    std::cerr << message << std::endl;
    std::exit(1);
  }
}

void run(const std::filesystem::path& dir, bool lateOutput) {
  const bool stageConflict = std::filesystem::exists(dir / "overload.mp4.partial");
  StudioOptions o;
  o.software_gpu = true;
  studio_detail::StudioGraphState state(o);
  StudioSource source;
  source.id = "sequence";
  state.scene.sources.push_back(source);
  std::promise<void> entered, unblock;
  auto release = unblock.get_future().share();
  std::atomic<unsigned> writes{0};
  state.beforeRecordWrite = [&] {
    if (++writes == 1) {
      entered.set_value();
      require(release.wait_for(15s) == std::future_status::ready,
              "Studio tick waited for recorder");
    }
  };

  state.recordPath = dir / "overload.mp4";
  state.recordWanted = true;
  auto feed = [&](int index) {
    std::lock_guard lock(state.mutex);
    auto& samples = state.remoteAudio["synthetic"];
    samples.clear();
    for (int i = 0; i < 9600; ++i) {
      const float v = .3f * std::sin(6.2831853f * (index >= 7 ? 3000 : 500) * i / 48000.f);
      samples.push_back(v);
      samples.push_back(-v);
    }
  };
  feed(0);
  studio_detail::StudioGraph graph(state);
  require(bool(graph.start()), "Studio graph failed to start");
  auto now = Clock::now();
  auto step = [&](int index) {
    const float level = float(25 + index * 16) / 255;
    {
      std::lock_guard lock(state.mutex);
      state.scene.sources[0].color = {level, level, level, 1};
    }
    feed(index);
    // Keep the synthetic source clock within the real PCM input contract.
    std::this_thread::sleep_until(now);
    auto tick = graph.step(now);
    require(bool(tick), tick ? "" : tick.error().message.c_str());
    now += 33333us;
  };
  auto count = [&](const char* name) -> uint64_t {
    for (const auto& n : graph.nodes()) {
      if (n.name == name) {
        return n.received;
      }
    }
    std::exit(2);
  };
  step(0);
  require(entered.get_future().wait_for(2s) == std::future_status::ready, "Encoder never entered");
  for (int i = 1; i < 6; ++i) {
    step(i);
    require(state.status.recording && !state.status.failure, "Premature recording failure");
  }
  step(6);  // First event in flight is audio; six video events are already queued.
  const auto original = state.status.failure;
  require(original && original->code == ErrorCode::resource_exhausted && !state.recordWanted &&
              !state.status.recording && state.recordBusy,
          "Expected session-only backlog error");
  auto audio = count("studio.audio-output"), video = count("studio.video-output");
  auto monitor = count("studio.monitor"), preview = state.status.frames;
  for (int i = 7; i < 12; ++i) {
    step(i);
  }
  require(writes == 1 && count("studio.audio-output") > audio &&
              count("studio.monitor") > monitor && count("studio.video-output") == video + 5 &&
              state.status.frames == preview + 5 && state.status.preview &&
              state.status.failure == original,
          "Live outputs did not advance while the encoder was blocked");
  if (lateOutput) {
    require(MoveFileW((dir / "other-session.mp4").c_str(), (dir / "overload.mp4").c_str()),
            "Could not stage a competing publication after recording started");
  }
  unblock.set_value();
  const auto deadline = Clock::now() + 10s;
  while (state.recordBusy && Clock::now() < deadline) {
    require(bool(graph.step(now - 33333us)), "Failed to poll drain");
    std::this_thread::sleep_for(1ms);
  }
  require(!state.recordBusy && writes == state.finalizedRecordEvents && writes > 6 &&
              original == state.status.failure,
          "Did not drain exactly the accepted A/V prefix or lost first error");
  if (stageConflict || lateOutput) {
    require(state.status.error.find("; cleanup: ") != std::string::npos,
            "Remux failure did not survive alongside original overload");
  } else {
    require(state.status.error == original->message, "Unexpected secondary failure");
  }
  state.beforeRecordWrite = {};
  state.status.failure.reset();  // Same reset performed by Studio::start_recording.
  state.status.error.clear();
  {
    std::lock_guard lock(state.mutex);
    state.recordPath = dir / "next.mp4";
    state.recordWanted = true;
  }
  // Open on a control-only tick. Frame 12 must be captured after the new session is ready.
  require(bool(graph.step(now - 33333us)), "Failed to prepare the next session");
  now = (std::max)(now + 33333us, Clock::now());
  step(12);
  step(13);
  {
    std::lock_guard lock(state.mutex);
    state.recordWanted = false;
  }
  step(14);
  graph.stop();
  graph.release();
  require(!state.recordBusy && !state.status.failure, "Old recording affected the next session");
}
}  // namespace

int main(int argc, char** argv) {
  const bool lateOutput = argc == 3 && std::string(argv[2]) == "--late-output";
  require((argc == 2 || lateOutput) && SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "Setup failed");
  run(std::filesystem::u8path(argv[1]), lateOutput);
  CoUninitialize();
}
