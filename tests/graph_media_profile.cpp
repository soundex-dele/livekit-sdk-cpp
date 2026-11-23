#include "graph_media_profile_round.hpp"
#include "engines/player/playback_graph.hpp"
#include "engines/studio/internal.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <map>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
using namespace livekit::sdk;
using detail::PlaybackGraph;
using studio_detail::Recorder;
namespace gf = graphflow;
constexpr int width = 1280, height = 720, fps = 25, samples = 48000 / fps;
constexpr auto period = 40ms;

int64_t us(Clock::duration value) {
  return std::chrono::ceil<std::chrono::microseconds>(value).count();
}

struct Times {
  std::vector<int64_t> values;

  void add(Clock::duration v) {
    values.push_back(us(v));
  }

  Json report() const {
    auto v = values;
    std::sort(v.begin(), v.end());
    auto n = v.size();
    return {{"samples", n},
            {"p95Us", n ? v[n - n / 20 - 1] : 0},
            {"p99Us", n ? v[n - n / 100 - 1] : 0},
            {"maxUs", n ? v.back() : 0}};
  }
};

Json latency(const gf::LatencySnapshot& s) {
  return {{"samples", s.samples}, {"p95Us", s.p95Us}, {"p99Us", s.p99Us}, {"maxUs", s.maxUs}};
}

#define CHECK(x)                             \
  do {                                       \
    if (!(x)) {                              \
      std::cerr << __LINE__ << ": " #x "\n"; \
      return false;                          \
    }                                        \
  } while (false)

struct Event {
  int seq = 0;
  Clock::time_point scheduled;
  StudioImage image;
  std::array<float, samples * 2> audio;
};

std::shared_ptr<const Event> generate(int index, Clock::time_point scheduled) {
  auto p = std::make_shared<Event>();
  p->seq = index;
  p->scheduled = scheduled;
  auto& im = p->image;
  im.width = width;
  im.height = height;
  im.rgba.resize(width * height * 4);
  for (int y = 0; y < height; ++y) {
    auto dst = im.rgba.data() + y * width * 4;
    for (int x = 0; x < width; ++x, dst += 4) {
      dst[0] = static_cast<uint8_t>(x + 3 * index);
      dst[1] = static_cast<uint8_t>(y + 2 * index);
      dst[2] = static_cast<uint8_t>(((x + index) / 32 ^ (y / 32)) * 31);
      dst[3] = 255;
      if (x < 128 && y < 16)
        dst[0] = dst[1] = dst[2] = (index & (1 << (x / 16))) ? 224 : 32;
    }
  }
  for (int i = 0; i < samples; ++i) {
    auto v = static_cast<float>(
        .3 * std::sin((index % 2 ? 880. : 440.) * i * 6.283185307179586 / 48000.));
    p->audio[2 * i] = v;
    p->audio[2 * i + 1] = -v;
  }
  return p;
}

struct Recorded {
  int count = 0, missed = 0;
  Times ready;
  Clock::time_point firstFrame, lastFrame;
};

struct RecordNode : public gf::INode {
  std::filesystem::path file;
  Recorded& result;
  std::unique_ptr<Recorder> encoder;

  RecordNode(std::filesystem::path path, Recorded& out) : file(std::move(path)), result(out) {}

  static gf::Status status(const Result<void>& r) {
    if (!r)
      return {{gf::ErrorCode::NodeFailure, r.error().message, r.failure_record()}};
    return {};
  }

  gf::Status start() override {
    encoder = std::make_unique<Recorder>(file, width, height, fps);
    return status(encoder->status());
  }

  void bind(gf::Binder& b) override {
    b.input(gf::In<Event>{"frame", gf::Connections::ExactlyOne},
            [this](const Event& e) -> gf::Status {
              if (result.count != e.seq)
                return {{gf::ErrorCode::NodeFailure, "recording sequence mismatch"}};
              if (!result.count) {
                result.firstFrame = Clock::now();
              }
              if (auto r = encoder->video(e.image, e.scheduled); !r)
                return status(r);
              if (auto r = encoder->audio(e.audio, e.scheduled); !r)
                return status(r);
              ++result.count;
              result.lastFrame = Clock::now();
              auto elapsed = result.lastFrame - e.scheduled;
              result.ready.add(elapsed);
              if (elapsed > period)
                ++result.missed;
              return {};
            });
  }

  gf::Status stop() override {
    return status(encoder->finish());
  }
};

struct PreviewStats {
  int seen = 0, last = -1;
  std::uint64_t checksum = 0;
};

struct Preview : public gf::INode {
  PreviewStats& stats;

  explicit Preview(PreviewStats& s) : stats(s) {}

  void bind(gf::Binder& b) override {
    b.input(gf::In<Event>{"frame"}, [this](const Event& e) -> gf::Status {
      if (e.seq <= stats.last)
        return {{gf::ErrorCode::NodeFailure, "preview reordered"}};
      ++stats.seen;
      stats.last = e.seq;
      // CPU thumbnail sampling, not presentation to a window/display.
      for (int y = 0; y < height; y += 8)
        for (int x = 0; x < width; x += 8)
          stats.checksum += e.image.rgba[(y * width + x) * 4];
      return {};
    });
  }
};

bool record(const std::filesystem::path& file, int frames, Json& report, const MediaPhaseReady& ready) {
  Recorded disk;
  PreviewStats preview;
  gf::Graph g({gf::Dispatch::Queued, 4, true});
  auto src = g.ingress<Event>("software.capture");
  auto p = g.add<Preview>("preview", preview);
  auto r = g.add<RecordNode>("file.record", file, disk);
  CHECK(src && p && r && g.rewire([&](auto& w) {
    w.connect(src->port, p->in(gf::In<Event>{"frame"}), {}, {gf::DeliveryPolicy::Latest, 1});
    w.connect(src->port, r->in(gf::In<Event>{"frame"}), {}, {gf::DeliveryPolicy::Reliable, 4});
  }));
  auto t = Clock::now();
  CHECK(g.start());
  report["startUs"] = us(Clock::now() - t);
  CHECK(ready());  // All encoders open, before the measured capture window.
  const auto base = Clock::now();
  report["startTickUs"] = us(base.time_since_epoch());
  Times generation, wakeup, admission;
  std::uint64_t full = 0, evicted = 0, skipped = 0;
  for (int n = 0; n < frames; ++n) {
    const auto due = base + n * period;
    std::this_thread::sleep_until(due);
    auto begin = Clock::now();
    wakeup.add(begin - due);
    auto e = generate(n, due);
    generation.add(Clock::now() - begin);
    auto post = Clock::now();
    for (;;) {
      auto sent = src->writer.emit(e);
      if (sent.status == gf::Admission::Accepted ||
          sent.status == gf::Admission::AcceptedWithDrops) {
        evicted += sent.evicted;
        skipped += sent.skipped.size();
        break;
      }
      CHECK(sent.retryable() && Clock::now() - post < 5s);
      ++full;
      std::this_thread::yield();
    }
    admission.add(Clock::now() - post);
  }
  t = Clock::now();
  report["endTickUs"] = us(t.time_since_epoch());
  auto stopped = g.stop(t + 5s);
  if (!stopped)
    std::cerr << stopped.error.message << '\n';
  CHECK(stopped);
  report["stopAndFinalizeUs"] = us(Clock::now() - t);
  report["captureToFinalizedUs"] = us(Clock::now() - base);
  CHECK(disk.count == frames && preview.seen + evicted + skipped == static_cast<unsigned>(frames) &&
        preview.checksum);
  auto view = g.snapshot();
  CHECK(view);
  for (const auto& n : view->nodes)
    if (!n.ingress) {
      CHECK(!n.failed && !n.waiting && !n.executing &&
            n.execution.samples ==
                static_cast<unsigned>(n.id == "preview" ? preview.seen : disk.count));
      report["nodes"][n.id] = {{"queueWait", latency(n.queueWait)},
                               {"execution", latency(n.execution)},
                               {"peakWaiting", n.peakWaiting}};
    }
  report["wakeAfterSchedule"] = wakeup.report();
  report["generate"] = generation.report();
  report["admission"] = admission.report();
  report["scheduleToEncoderReturn"] = disk.ready.report();
  report["afterFramePeriod"] = disk.missed;
  report["firstMediaTickUs"] = us(disk.firstFrame.time_since_epoch());
  report["lastMediaTickUs"] = us(disk.lastFrame.time_since_epoch());
  report["recorded"] = disk.count;
  report["previewed"] = preview.seen;
  report["evicted"] = evicted;
  report["skipped"] = skipped;
  report["fullRetries"] = full;
  CHECK(g.close());
  return true;
}

int barcode(const media::VideoFrame& frame) {
  if (frame.width != width || frame.height != height || !frame.rgba || frame.stride < width * 4 ||
      frame.rgba->size() < size_t(frame.stride) * height)
    return -1;
  int value = 0;
  for (int i = 0; i < 8; ++i)
    if ((*frame.rgba)[8 * frame.stride + (i * 16 + 8) * 4] > 128)
      value |= 1 << i;
  return value;
}

struct Decoded {
  int video = 0, audio = 0;
  int64_t samplesSeen = 0, lastVideo = -1, lastAudio = -1;
  bool valid = true;

  void accept(const media::VideoFrame& f) {
    valid &= barcode(f) == (video % 256) && f.timestamp_us > lastVideo;
    lastVideo = f.timestamp_us;
    ++video;
  }

  void accept(const media::AudioFrame& f) {
    valid &= f.pcm && f.channels == 2 && f.sample_rate == 48000 && f.samples > 0 &&
             f.pcm->size() == size_t(f.samples) * 2 && f.timestamp_us >= lastAudio;
    lastAudio = f.timestamp_us;
    ++audio;
    samplesSeen += f.samples;
  }
};

bool decodeFast(const std::string& path, int frames, Json& report) {
  Decoded out;
  std::atomic<bool> cancelled{false};
  PlaybackGraph g(cancelled, {});
  auto t = Clock::now();
  CHECK(g.open(path));
  report["openUs"] = us(Clock::now() - t);
  auto outputs = g.outputs();
  outputs->flush(1);
  CHECK(g.start());
  Times step;
  std::map<std::string, Times> pumps, progress;
  const detail::MediaExecutor::PumpProbe probe = [&](const std::string& id, Clock::duration elapsed,
                                                     bool advanced) {
    pumps[id].add(elapsed);
    if (advanced)
      progress[id].add(elapsed);
  };
  const auto begin = Clock::now();
  for (;;) {
    t = Clock::now();
    auto s = g.step(probe);
    step.add(Clock::now() - t);
    CHECK(s);
    while (auto v = outputs->video->take_due(INT64_MAX, 1))
      out.accept(*v);
    while (auto a = outputs->audio->take_due(INT64_MAX, 1))
      out.accept(*a);
    if (*s == PlaybackGraph::Step::end)
      break;
    CHECK(Clock::now() - begin < 60s);
  }
  report["decodeUs"] = us(Clock::now() - begin);
  CHECK(out.valid && out.video == frames && out.samplesSeen >= int64_t(frames - 1) * samples &&
        out.samplesSeen <= int64_t(frames + 8) * samples);
  report["videoFrames"] = out.video;
  report["stereoSamples"] = out.samplesSeen;
  report["sweep"] = step.report();
  for (const auto& item : pumps)
    report["nodes"][item.first] = {{"pump", item.second.report()},
                                   {"progressPump", progress[item.first].report()}};
  CHECK(pumps.size() == 7 && g.pause());
  return true;
}

bool playClocked(const std::string& path, int frames, Json& report, const MediaPhaseReady& phaseReady) {
  Decoded out;
  Times video, audio;
  Clock::time_point start, firstFrame, lastFrame;
  std::promise<PlayerState> end;
  bool signalled = false;
  int missed = 0;
  Player player;
  CHECK(player.open(path).get());
  PlayerCallbacks callbacks;
  callbacks.video = [&](const media::VideoFrame& f) {
    lastFrame = Clock::now();
    if (!out.video) {
      firstFrame = lastFrame;
    }
    auto lateness = lastFrame - start - std::chrono::microseconds(f.timestamp_us);
    video.add(std::max(lateness, Clock::duration{}));
    if (lateness > period)
      ++missed;
    out.accept(f);
    return Result<void>::success();
  };
  callbacks.audio = [&](const media::AudioFrame& f) {
    auto lateness = Clock::now() - start - std::chrono::microseconds(f.timestamp_us);
    audio.add(std::max(lateness, Clock::duration{}));
    out.accept(f);
    return Result<void>::success();
  };
  callbacks.state = [&](const PlayerSnapshot& s) {
    if (!signalled && (s.state == PlayerState::ended || s.state == PlayerState::failed)) {
      signalled = true;
      end.set_value(s.state);
    }
    return Result<void>::success();
  };
  auto ready = end.get_future();
  CHECK(player.set_callbacks(std::move(callbacks), {PlayerAudioDelivery::clocked, 0us}));
  CHECK(phaseReady());  // Do not include waiting for a slower independent decode in playback.
  start = Clock::now();
  CHECK(player.play().get());
  CHECK(ready.wait_for(frames * period + 10s) == std::future_status::ready &&
        ready.get() == PlayerState::ended);
  auto done = Clock::now();
  CHECK(player.close().get());
  report["closeUs"] = us(Clock::now() - done);
  CHECK(out.valid && out.video == frames && out.audio > 0);
  report["startTickUs"] = us(start.time_since_epoch());
  report["endTickUs"] = us(done.time_since_epoch());
  report["firstMediaTickUs"] = us(firstFrame.time_since_epoch());
  report["lastMediaTickUs"] = us(lastFrame.time_since_epoch());
  report["playToEndUs"] = us(done - start);
  report["videoLate"] = video.report();
  report["audioLate"] = audio.report();
  report["videoOverOnePeriod"] = missed;
  report["stereoSamples"] = out.samplesSeen;
  return true;
}
}  // namespace

int mediaProfile(const std::filesystem::path& root, int frames, const MediaPhaseReady& ready) {
  if (frames < 25 || frames > 1500)
    return 2;
  const auto path = root / "profile.mp4";
  Json doc;
  doc["profile"] = "windows-software-av-720p25-v1";
  doc["width"] = width;
  doc["height"] = height;
  doc["fps"] = fps;
  doc["sourceFrames"] = frames;
  if (!record(path, frames, doc["record"], ready) || !decodeFast(path.u8string(), frames, doc["decode"]) ||
      !playClocked(path.u8string(), frames, doc["playback"], ready))
    return 1;
  if (doc["decode"]["stereoSamples"] != doc["playback"]["stereoSamples"])
    return 1;
  std::ofstream file(root / "media-metrics.json");
  file << doc.dump(2) << '\n';
  file.close();
  return file ? 0 : 1;
}

int mediaProfile(const std::filesystem::path& root, int frames) {
  return mediaProfile(root, frames, [] { return true; });
}
