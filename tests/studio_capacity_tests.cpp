#include "engines/studio/studio_graph.hpp"
#include "media_nodes/capture/audio_source.hpp"
#include "media_nodes/capture/video_source.hpp"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace livekit::sdk;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

int number(std::string_view arg) {
  int v = 0;
  auto parsed = std::from_chars(arg.data(), arg.data() + arg.size(), v);
  return parsed.ec == std::errc{} && parsed.ptr == arg.data() + arg.size() ? v : -1;
}

long long us(Clock::duration duration) {
  return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

std::shared_ptr<const StudioImage> frame(uint64_t seq) {
  auto image = std::make_shared<StudioImage>();
  image->width = 320;
  image->height = 180;
  image->rgba.resize(320 * 180 * 4);
  for (int y = 0; y < 180; ++y) {
    for (int x = 0; x < 320; ++x) {
      // The header survives resize/H.264. The moving body exercises composition.
      const auto level =
          y < 60 ? ((seq >> (x / 40)) & 1 ? 224 : 32) : (x + 2 * y + 5 * seq) % 224 + 16;
      const auto i = (y * 320 + x) * 4;
      image->rgba[i] = image->rgba[i + 1] = image->rgba[i + 2] = static_cast<uint8_t>(level);
      image->rgba[i + 3] = 255;
    }
  }
  return image;
}

// Experimental A/B control, never changes the system timer resolution. Refuse
// an unavailable experiment instead of silently reporting the baseline as B.
struct PollExperiment {
  HANDLE timer = nullptr;

  explicit PollExperiment(bool precise) {
    if (precise) {
      timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                     TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
  }

  ~PollExperiment() {
    if (timer) {
      CloseHandle(timer);
    }
  }

  bool wait() const {
    if (!timer) {
      std::this_thread::sleep_for(2ms);  // Studio::Impl::run's production behavior.
      return true;
    }
    LARGE_INTEGER due{};
    due.QuadPart = -20000;  // Relative 2 ms, 100 ns units.
    return SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) &&
           WaitForSingleObject(timer, 1000) == WAIT_OBJECT_0;
  }
};

int64_t processCpuUs() {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
    return -1;
  }
  auto ticks = [](FILETIME t) { return (int64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  return (ticks(kernel) + ticks(user)) / 10;
}

bool profile(const std::filesystem::path& path, int width, int height, int seconds, bool trace,
             bool precise, bool holdVideo, bool slowOpen, bool hotOpen, bool starveFeed) {
  PollExperiment poll(precise);
  if (precise && !poll.timer) {
    return false;
  }
  StudioOptions options;
  options.width = width;
  options.height = height;
  options.software_gpu = true;
  studio_detail::StudioGraphState state(options);
  StudioSource remote;
  remote.id = remote.input = "fixture";
  remote.kind = StudioSourceKind::remote;
  state.scene.sources = {remote};
  std::array<float, 960> tone{};
  for (int i = 0; i < 480; ++i) {
    tone[2 * i] = .3f * std::sin(6.2831853f * i / 96.f);
    tone[2 * i + 1] = -tone[2 * i];
  }
  for (int i = 0; i < 10; ++i)
    state.remoteAudio[remote.id].insert(state.remoteAudio[remote.id].end(), tone.begin(),
                                        tone.end());
  state.remoteVideo[remote.id] = frame(0);
  state.recordPath = path / "take.mp4";
  state.recordWanted = !hotOpen;
  const auto origin = Clock::now();
  auto ns = [&](Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time - origin).count();
  };
  // One writer per vector; no file I/O or shared lock on the hot path. The
  // benchmark has a bounded duration and a bounded number of samples per tick.
  std::vector<std::array<int64_t, 5>> writes;
  std::vector<std::array<int64_t, 4>> reads;
  std::vector<std::array<int64_t, 2>> videos, steps;
  int64_t holdBegin = 0, holdEnd = 0;
  int64_t openBegin = 0, openEnd = 0, request = 0, epoch = 0;
  uint64_t firstRecordFrame = 0;
  if (trace) {
    state.traceRecordReady = [&](auto requested, auto ready) {
      request = ns(requested);
      epoch = ns(ready);
    };
  }
  if (slowOpen) {
    state.beforeRecordOpen = [&] {
      openBegin = ns(Clock::now());
      std::this_thread::sleep_for(180ms);
      openEnd = ns(Clock::now());
    };
  }
  std::vector<std::array<int64_t, 4>> mixContent;
  std::vector<std::array<int64_t, 2>> encodedContent;
  auto classify = [&](const std::vector<float>& pcm) {
    if (pcm.size() == tone.size()) {
      if (std::equal(pcm.begin(), pcm.end(), tone.begin())) {
        return 1;  // Exact fixture samples, both channels.
      }
      if (std::all_of(pcm.begin(), pcm.end(), [](float x) { return x == 0; })) {
        return 0;
      }
    }
    return -1;  // Never classify a malformed or partially silent slot as a good tone.
  };
  std::atomic<bool> pauseFeeder{false};
  if (trace) {
    mixContent.reserve((seconds + 2) * 100);
    encodedContent.reserve((seconds + 2) * 100);
    state.traceMix = [&](auto slot, size_t supplied, const auto& remotePcm, const auto& program) {
      mixContent.push_back({ns(slot), int64_t(supplied), classify(remotePcm), classify(program)});
      if (starveFeed && mixContent.size() == 40) {
        pauseFeeder = true;
      }
    };
    state.traceEncoderAudio = [&](auto slot, const auto& pcm) {
      encodedContent.push_back({ns(slot), classify(pcm)});
    };
    writes.reserve((seconds + 2) * 100);
    reads.reserve((seconds + 2) * 100);
    videos.reserve((seconds + 2) * 30);
    steps.reserve((seconds + 2) * 500);
    state.traceProgramWrite = [&](auto captured, auto mixed, auto enter, auto leave, auto status) {
      writes.push_back({ns(captured), ns(mixed), ns(enter), ns(leave),
                        status == AudioWriteStatus::accepted ? 1 : 0});
    };
    state.traceVideoDispatch = [&](auto enter, auto leave) {
      videos.push_back({ns(enter), ns(leave)});
      if (holdVideo && !holdBegin && videos.size() == 4) {
        holdBegin = ns(Clock::now());
        std::this_thread::sleep_for(300ms);
        holdEnd = ns(Clock::now());
      }
    };
  }
  std::vector<std::array<int64_t, 4>> refills;
  if (trace) {
    refills.reserve((seconds + 3) * 100);
  }
  // A remote publisher does not run on the compositor thread. Keep a bounded PCM FIFO
  // fed even during the deliberate video stall, so an empty fixture is not mistaken for graph loss.
  livekit::compat::jthread input([&](livekit::compat::stop_token stop) {
    while (!stop.stop_requested()) {
      if (starveFeed && pauseFeeder.exchange(false)) {
        std::this_thread::sleep_for(300ms);  // Negative control: leave the existing FIFO intact.
      }
      const auto attempt = trace ? Clock::now() : Clock::time_point{};
      {
        std::lock_guard lock(state.mutex);
        const auto acquired = trace ? Clock::now() : Clock::time_point{};
        auto& queue = state.remoteAudio[remote.id];
        const auto before = queue.size();
        while (queue.size() < 9600)
          queue.insert(queue.end(), tone.begin(), tone.end());
        if (trace) {
          refills.push_back({ns(attempt), ns(acquired), int64_t(before), int64_t(queue.size())});
        }
      }
      std::this_thread::sleep_for(10ms);
    }
  });
  studio_detail::StudioGraph graph(state);
  auto running = graph.start();
  if (!running) {
    std::cerr << running.error().message << '\n';
    return false;
  }
  // Consume the same I420 / PCM boundaries as a publisher, without a room/network.
  detail::I420SourceReader video(state.programVideo);
  detail::PcmSourceReader audio(state.programAudio);
  if (!video.start() || !audio.start()) {
    return false;
  }
  uint64_t markerGaps = 0, badVideo = 0, badAudio = 0, toneBlocks = 0;
  std::thread videoThread([&] {
    unsigned expected = 0;
    while (auto data = video.read({})) {
      const auto& f = data->frame;
      if (f.width != width || f.height != height || f.y.size() < size_t(f.stride_y) * height) {
        ++badVideo;
        continue;
      }
      unsigned id = 0;
      for (int bit = 0; bit < 8; ++bit) {
        auto y = f.y[(height / 8) * f.stride_y + (2 * bit + 1) * width / 16];
        if (y > 80 && y < 176) {
          ++badVideo;
        }
        id |= unsigned(y >= 176) << bit;
      }
      markerGaps += id != expected;
      expected = (id + 1) % 256;
    }
  });
  std::thread audioThread([&] {
    auto next = Clock::now();
    bool first = true;
    for (;;) {
      std::this_thread::sleep_until(next);
      const auto late = Clock::now() - next;
      if (!first && late > 20ms) {
        const auto skipped = late / 10ms;
        audio.skip(static_cast<size_t>(skipped));
        next += skipped * 10ms;
      }
      const auto enter = trace ? Clock::now() : Clock::time_point{};
      auto block = audio.read();
      const auto leave = trace ? Clock::now() : Clock::time_point{};
      if (!block) {
        break;
      }
      if (trace) {
        reads.push_back({ns(block->captured), ns(enter), ns(leave), block->silence ? 1 : 0});
      }
      if (first) {
        next = Clock::now();
        first = false;
      }
      if (!block->silence) {
        if (block->samples.size() != 960 || block->samples[48] < 8000 ||
            block->samples[49] > -8000) {
          ++badAudio;
        } else {
          ++toneBlocks;
        }
      }
      next += 10ms;
    }
  });
  std::vector<long long> times;
  uint64_t ticks = 0;
  Failure failure;
  const auto cpuBegin = processCpuUs();
  const auto beginMs = GetTickCount64();
  const auto begin = Clock::now();
  const auto deadline = begin + std::chrono::seconds(seconds);
  do {
    const auto before = state.status.frames;
    const auto start = Clock::now();
    const bool opening = hotOpen && !openBegin && start - begin >= 100ms;
    if (opening) {
      std::lock_guard lock(state.mutex);
      state.recordWanted = true;
    }
    auto result = graph.step(start);
    if (opening) {
      firstRecordFrame = state.status.frames;
    }
    const auto end = Clock::now();
    if (trace) {
      steps.push_back({ns(start), ns(end)});
    }
    ++ticks;
    if (!result) {
      failure = result.failure_record();
      break;
    }
    if (state.status.frames > before) {
      times.push_back(us(end - start));
      state.remoteVideo[remote.id] = frame(state.status.frames);
    }
    if (!poll.wait()) {
      failure =
          make_failure(ErrorDomain::media, ErrorCode::processing_failed, "poll timer wait failed");
      break;
    }
  } while (Clock::now() < deadline);
  const auto end = Clock::now();
  const auto endMs = GetTickCount64();
  const auto cpuEnd = processCpuUs();
  const auto liveAudio = state.programAudio->stats();
  auto stop = Clock::now();
  graph.stop();
  const auto stopped = Clock::now();
  Json nodes = Json::object();
  for (const auto& n : graph.nodes()) {
    nodes[n.name] = n.received;
  }

  graph.release();
  const auto released = Clock::now();
  input.request_stop();
  input.join();
  video.stop();
  audio.stop();
  videoThread.join();
  audioThread.join();
  const auto vs = state.programVideo->stats();
  const auto as = state.programAudio->stats();
  std::sort(times.begin(), times.end());
  auto percentile = [&](size_t p) {
    return times.empty() ? 0LL : times[(times.size() * p + 99) / 100 - 1];
  };
  Json report = {{"width", width},
                 {"height", height},
                 {"targetFps", options.fps},
                 {"beginMs", beginMs},
                 {"endMs", endMs},
                 {"runUs", us(end - begin)},
                 {"ticks", ticks},
                 {"pollMode", precise ? "experimental-precise" : "production"},
                 {"processCpuUs", cpuBegin >= 0 && cpuEnd >= cpuBegin ? cpuEnd - cpuBegin : -1},
                 {"previewFrames", state.status.frames},
                 {"dropped", state.status.dropped},
                 {"received", nodes},
                 {"programVideo",
                  {{"accepted", vs.accepted},
                   {"consumed", vs.consumed},
                   {"rejected", vs.rejected},
                   {"dropped", vs.dropped},
                   {"markerGaps", markerGaps},
                   {"badMarkers", badVideo}}},
                 {"programAudio",
                  {{"acceptedSamples", as.accepted_samples},
                   {"consumedSamples", as.consumed_samples},
                   {"rejectedWrites", as.rejected_writes},
                   {"droppedSamples", as.dropped_samples},
                   {"activeSilenceSamples", liveAudio.silence_samples},
                   {"activeSkippedSamples", liveAudio.skipped_samples},
                   {"verifiedBlocks", toneBlocks},
                   {"badBlocks", badAudio}}},
                 {"videoTickP95Us", percentile(95)},
                 {"videoTickP99Us", percentile(99)},
                 {"videoTicksOverPeriod",
                  std::count_if(times.begin(), times.end(), [](auto v) { return v > 33333; })},
                 {"graphStopUs", us(stopped - stop)},
                 {"graphReleaseUs", us(released - stopped)},
                 {"recordBusyAfterRelease", state.recordBusy},
                 {"overloaded", state.status.failure &&
                                    state.status.failure->code == ErrorCode::resource_exhausted},
                 {"recordError", state.status.error},
                 {"graphError", failure ? failure->message : ""}};
  if (trace) {
    auto save = [&](const char* name, const char* columns, const auto& rows) {
      std::ofstream file(path / name);
      file << columns << '\n';
      for (const auto& row : rows) {
        for (size_t i = 0; i < row.size(); ++i) {
          if (i) {
            file << ',';
          }
          file << row[i];
        }
        file << '\n';
      }
      file.close();
      return bool(file);
    };
    if (!save("pcm-refill.csv", "attempt,acquired,before,after", refills) ||
        !save("pcm-mix-content.csv", "slot,supplied,remote,program", mixContent) ||
        !save("pcm-encoder-content.csv", "slot,encoder", encodedContent) ||
        !save("pcm-write.csv", "slot,mixed,enter,leave,accepted", writes) ||
        !save("pcm-read.csv", "slot,enter,leave,silence", reads) ||
        !save("video-dispatch.csv", "enter,leave", videos) ||
        !save("graph-step.csv", "enter,leave", steps)) {
      return false;
    }
    if (holdVideo)
      report["videoOwnerHoldNs"] = {holdBegin, holdEnd};
    if (slowOpen) {
      report["recordOpenHoldNs"] = {openBegin, openEnd};
      report["recordRequestNs"] = request;
      report["firstRecordFrame"] = firstRecordFrame;
    }
    report["fixtureFifoFinalFloats"] = state.remoteAudio.at(remote.id).size();
    report["recordEpochNs"] = epoch;
    report["traceRunEndNs"] = ns(end);
    report["totalSilenceSamples"] = as.silence_samples;
  }
  std::ofstream file(path / "metrics.json");
  file << report.dump(2) << '\n';
  file.close();
  return bool(file) && !failure && !state.recordBusy && !state.status.failure;
}
}  // namespace

int main(int argc, char** argv) {
  bool trace = false, precise = false, hold = false, slowOpen = false, hotOpen = false, starveFeed = false;
  if (argc < 5 || argc > 8) {
    return 2;
  }
  for (int i = 5; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--pcm-trace" && !trace) {
      trace = true;
    } else if (std::string_view(argv[i]) == "--hold-video" && !hold) {
      hold = true;
    } else if (std::string_view(argv[i]) == "--starve-feed" && !starveFeed) {
      starveFeed = true;
    } else if (std::string_view(argv[i]) == "--slow-open" && !slowOpen) {
      slowOpen = true;
    } else if (std::string_view(argv[i]) == "--slow-hot-open" && !slowOpen) {
      slowOpen = hotOpen = true;
    } else if (std::string_view(argv[i]) == "--precise-poll" && !precise) {
      precise = true;
    } else {
      return 2;
    }
  }
  const int width = number(argv[2]), height = number(argv[3]), seconds = number(argv[4]);
  if (seconds < 2 || seconds > 180 || FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
    return 2;
  }
  const bool ok = (!(hold || slowOpen || starveFeed) || trace) &&
                  profile(std::filesystem::u8path(argv[1]), width, height, seconds, trace, precise,
                          hold, slowOpen, hotOpen, starveFeed);
  CoUninitialize();
  return ok ? 0 : 1;
}
