#include "engines/studio/internal.hpp"
#include "engines/studio/recording_session.hpp"
#include "media_nodes/capture/audio_source.hpp"
#include <graphflow/graph.hpp>
#include <windows.h>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string_view>
#include <thread>

namespace {
using namespace livekit::sdk;
using namespace std::chrono_literals;
namespace gf = graphflow;
namespace sd = studio_detail;
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
constexpr int kBlocks = 20;

void require(bool ok, const char* message) {
  if (!ok) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

struct Frame {
  int seq;
  Time captured;
};

struct Pcm {
  Time captured, submitted;
  std::vector<float> samples;
};

struct Fixture {
  sd::RecordingSession& recording;
  sd::Compositor& compositor;
  PcmAudioSource& output;
  bool holdVideo;
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, resume = false;
  int videoCount = 0, audioCount = 0;
  std::vector<std::array<int64_t, 2>> timing;

  bool hold() {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    return changed.wait_for(lock, 4s, [&] { return resume; });
  }

  void await(bool waiting, int audio, int video) {
    std::unique_lock lock(mutex);
    require(changed.wait_for(
                lock, 2s,
                [&] { return entered == waiting && audioCount >= audio && videoCount >= video; }),
            "independent branch did not advance before timeout");
  }

  void unblock() {
    std::lock_guard lock(mutex);
    resume = true;
    changed.notify_all();
  }
};

struct Composite : public gf::INode {
  Fixture& f;

  explicit Composite(Fixture& fixture) : f(fixture) {}

  void bind(gf::Binder& b) override {
    b.input(gf::In<Frame>{"layers", gf::Connections::ExactlyOne}, [this](const Frame& frame) {
      if (f.holdVideo && frame.seq == 0) {
        require(f.hold(), "timed out holding video");
      }
      StudioSource layer;
      layer.id = "marker";
      auto pixels = std::make_shared<StudioImage>();
      pixels->width = pixels->height = 2;
      pixels->rgba.resize(16, frame.seq == 0 ? 40 : 210);
      for (int i = 3; i < 16; i += 4) {
        pixels->rgba[i] = 255;
      }
      auto rendered = f.compositor.render({{layer, pixels}});
      require(bool(rendered), "WARP compositor failed");
      f.recording.video(std::make_shared<StudioImage>(std::move(*rendered)), frame.captured);
      std::lock_guard lock(f.mutex);
      ++f.videoCount;
      f.changed.notify_all();
    });
  }
};

struct Mix : public gf::INode {
  Fixture& f;

  explicit Mix(Fixture& fixture) : f(fixture) {}

  void bind(gf::Binder& b) override {
    b.input(gf::In<Pcm>{"pcm", gf::Connections::ExactlyOne}, [this](const Pcm& pcm) {
      if (!f.holdVideo && f.audioCount == 0) {
        require(f.hold(), "timed out holding audio");
      }
      PcmInputFrame out;
      out.capture_time = pcm.captured;
      out.float_samples.resize(pcm.samples.size());
      require(bool(studio_mix(out.float_samples, pcm.samples, 1)), "mix failed");
      auto accepted = f.output.write(out);
      require(accepted && *accepted == AudioWriteStatus::accepted, "program PCM rejected");
      const auto written = Clock::now();
      auto us = [](Clock::duration value) {
        return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
      };
      f.timing.push_back({us(pcm.submitted - pcm.captured), us(written - pcm.captured)});
      f.recording.audio(out.float_samples, pcm.captured);
      std::lock_guard lock(f.mutex);
      ++f.audioCount;
      f.changed.notify_all();
    });
  }
};

void run(const std::filesystem::path& directory, bool holdVideo) {
  StudioOptions options;
  options.software_gpu = true;
  sd::Compositor compositor(options.width, options.height, true);
  require(bool(compositor.status()), "initialize WARP");
  PcmSourceOptions opts;
  opts.channels = 2;
  opts.format = AudioSampleFormat::float32;
  auto output = std::make_shared<PcmAudioSource>(opts);
  detail::PcmSourceReader reader(output);
  require(bool(reader.start()), "attach program reader");
  const auto origin = Clock::now() + 200ms;
  sd::RecordingSession recording(directory / "take.mp4", options, [origin] { return origin; }, {});
  require(!recording.status().first, "start recording");
  Fixture f{recording, compositor, *output, holdVideo};
  gf::Graph branches({gf::Dispatch::Queued, 4});
  auto audio = branches.ingress<Pcm>("audio.clock");
  auto video = branches.ingress<Frame>("video.clock");
  auto mixer = branches.add<Mix>("program.mix", f);
  auto compose = branches.add<Composite>("program.compose", f);
  require(audio && video && mixer && compose && branches.rewire([&](auto& wiring) {
    wiring.connect(audio->port, mixer->in(gf::In<Pcm>{"pcm"}), {},
                   {gf::DeliveryPolicy::Reliable, 4});
    wiring.connect(video->port, compose->in(gf::In<Frame>{"layers"}), {},
                   {gf::DeliveryPolicy::Reliable, 2});
  }) && branches.start(),
          "start independent mailboxes");
  auto sendAudio = [&](int n) {
    Pcm pcm{origin + (holdVideo ? 100ms : 0ms) + n * 10ms, {}, std::vector<float>(960)};
    std::this_thread::sleep_until(pcm.captured);
    for (int i = 0; i < 480; ++i) {
      pcm.samples[i * 2] = .3f * std::sin(6.2831853f * i / 96.f);
      pcm.samples[i * 2 + 1] = -pcm.samples[i * 2];
    }
    pcm.submitted = Clock::now();
    require(audio->writer.emit(std::move(pcm)).accepted == 1, "audio ingress rejected");
  };
  auto verifyAudio = [&](int n) {
    auto p = reader.read();
    require(p && !p->silence && p->captured == origin + (holdVideo ? 100ms : 0ms) + n * 10ms &&
                p->samples.size() == 960 && p->samples[48] > 8000 && p->samples[49] < -8000,
            "incorrect program PCM slot/content");
  };
  auto sendVideo = [&](int n) {
    Frame v{n, origin + (holdVideo ? (n ? 333333us : 0us) : (n ? 300ms : 100ms))};
    std::this_thread::sleep_until(v.captured);
    require(video->writer.emit(v).accepted == 1, "video ingress rejected");
  };
  if (holdVideo) {
    sendVideo(0);
    f.await(true, 0, 0);
    // Pace ingress by its own clock, never by video completion or PCM readback.
    // The reader verifies delivered content; it is not a model of device playout.
    std::thread consumer([&] {
      for (int n = 0; n < kBlocks; ++n) {
        f.await(true, n + 1, 0);
        verifyAudio(n);
      }
    });
    for (int n = 0; n < kBlocks; ++n) {
      sendAudio(n);
    }
    consumer.join();
    require(f.videoCount == 0, "video escaped hold");
    f.unblock();
    sendVideo(1);
  } else {
    sendAudio(0);
    f.await(true, 0, 0);
    for (int n = 0; n < 2; ++n) {
      sendVideo(n);
      f.await(true, 0, n + 1);
    }
    require(f.audioCount == 0, "audio escaped hold");
    f.unblock();
    for (int n = 0; n < kBlocks; ++n) {
      if (n) {
        sendAudio(n);
      }
      f.await(true, n + 1, 2);
      verifyAudio(n);
    }
  }
  require(bool(branches.stop()) && bool(branches.close()), "branch shutdown failed");
  recording.wait();
  const auto status = recording.status();
  require(status.done && !status.first && !status.cleanup && f.videoCount == 2 &&
              output->stats().consumed_samples == kBlocks * 480,
          "recording or program delivery failed");
  std::ofstream evidence(directory / "pcm-timing-us.txt");
  for (auto row : f.timing) {
    evidence << row[0] << ' ' << row[1] << '\n';
  }
  evidence.close();
  require(bool(evidence), "save timing evidence");
}
}  // namespace

int main(int argc, char** argv) {
  require(argc == 3 && SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "invalid setup");
  const std::string_view mode(argv[2]);
  require(mode == "hold-video" || mode == "hold-audio", "invalid mode");
  run(std::filesystem::u8path(argv[1]), mode == "hold-video");
  CoUninitialize();
}
