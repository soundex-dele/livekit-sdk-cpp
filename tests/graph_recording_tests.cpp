#include "engines/studio/internal.hpp"
#include <graphflow/graph.hpp>
#include <windows.h>
#include <fstream>
#include <array>
#include <cmath>
#include <charconv>
#include <string_view>
#include <future>
#include <iostream>

using livekit::sdk::StudioImage;
using livekit::sdk::studio_detail::Recorder;
namespace gf = graphflow;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
constexpr int frames = 32, samples = 1920, width = 64, height = 48;

struct Capture {
  int index;
  StudioImage image;
  std::array<float, samples * 2> audio;
};

std::shared_ptr<const Capture> capture(int n) {
  auto frame = std::make_shared<Capture>();
  frame->index = n;
  auto& image = frame->image;
  image.width = width;
  image.height = height;
  image.rgba.resize(width * height * 4);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const auto i = (y * width + x) * 4;
      const auto level = static_cast<uint8_t>((n & (1 << (x / 8))) ? 224 : 32);
      image.rgba[i] = image.rgba[i + 1] = image.rgba[i + 2] = level;
      image.rgba[i + 3] = 255;
    }
  int hz = n < 16 ? 400 : n < frames - 1 ? 1200 : 1900;
  for (int i = 0; i < samples; ++i) {
    float v = static_cast<float>(0.3 * std::sin(6.283185307179586 * hz * i / 48000.0));
    frame->audio[2 * i] = v;
    frame->audio[2 * i + 1] = -v;
  }
  return frame;
}

gf::Status translated(const livekit::sdk::Result<void>& s) {
  return s ? gf::Status{}
           : gf::Status{{gf::ErrorCode::NodeFailure, s.error().message, s.failure_record()}};
}

struct Run {
  gf::Emitter<Capture> source;
  std::promise<void> previewEntered, recordEntered, unblockPreview, unblockRecord;
  std::shared_future<void> resumePreview = unblockPreview.get_future().share(),
                           resumeRecord = unblockRecord.get_future().share();
  int written = 0, shown = 0;
  bool tail = false;
  std::uint64_t evicted = 0;
  livekit::sdk::Failure finishError;

  bool send(int n) {
    auto p = capture(n);
    auto until = Clock::now() + 3s;
    do {
      auto r = source.emit(p);
      if (r.status == gf::Admission::Accepted && r.accepted == 2) {
        evicted += r.evicted;
        return true;
      }
      if (!r.retryable())
        return false;
      std::this_thread::yield();
    } while (Clock::now() < until);
    return false;
  }
};

struct Source : public gf::INode {
  Run& run;

  explicit Source(Run& r) : run(r) {}

  void bind(gf::Binder& b) override {
    run.source = b.output(gf::Out<Capture>{"av"});
  }

  gf::Status stop() override {
    const auto token = cancellationToken();
    if (token.stopRequested())
      return {};
    if (!(run.tail = run.send(frames - 1)))
      return {{gf::ErrorCode::DispatchFailure, "tail not admitted"}};
    return {};
  }
};

struct Preview : public gf::INode {
  Run& run;

  explicit Preview(Run& r) : run(r) {}

  void bind(gf::Binder& b) override {
    b.input(gf::In<Capture>{"av"}, [this](const Capture& f) {
      if (f.index == 0) {
        run.previewEntered.set_value();
        run.resumePreview.wait_for(5s);
      }
      ++run.shown;
    });
  }
};

struct Record : public gf::INode {
  Run& run;
  std::filesystem::path path;
  std::unique_ptr<Recorder> rec;
  Clock::time_point base;

  Record(Run& r, std::filesystem::path p) : run(r), path(std::move(p)) {}

  gf::Status start() override {
    rec = std::make_unique<Recorder>(path, width, height, 25);
    base = Clock::now();
    return translated(rec->status());
  }

  void bind(gf::Binder& b) override {
    b.input(gf::In<Capture>{"av", gf::Connections::ExactlyOne},
            [this](const Capture& f) -> gf::Status {
              if (f.index == 0) {
                run.recordEntered.set_value();
                run.resumeRecord.wait_for(5s);
              }
              if (run.written != f.index)
                return {{gf::ErrorCode::NodeFailure, "recording frame sequence broken"}};
              const auto time = base + 40ms * f.index;
              if (auto s = rec->video(f.image, time); !s)
                return translated(s);
              if (auto s = rec->audio(f.audio, time); !s)
                return translated(s);
              ++run.written;
              return {};
            });
  }

  gf::Status stop() override {
    auto s = rec->finish();
    if (!s)
      run.finishError = s.failure_record();
    return translated(s);
  }
};

#define CHECK(x)                             \
  do {                                       \
    if (!(x)) {                              \
      std::cerr << __LINE__ << ": " #x "\n"; \
      return false;                          \
    }                                        \
  } while (false)

bool recordFile(std::filesystem::path path, bool finalizationFails) {
  Run run;
  gf::Graph graph({gf::Dispatch::Queued, 4});
  auto src = graph.add<Source>("capture", run);
  auto p = graph.add<Preview>("preview", run);
  auto r = graph.add<Record>("file.record", run, path);
  CHECK(src && p && r && graph.rewire([&](auto& w) {
    auto out = src->out(gf::Out<Capture>{"av"});
    w.connect(out, p->in(gf::In<Capture>{"av"}), {}, {gf::DeliveryPolicy::Latest, 1});
    w.connect(out, r->in(gf::In<Capture>{"av"}), {}, {gf::DeliveryPolicy::Reliable, 2});
  }) && graph.start());
  CHECK(run.send(0));
  CHECK(run.previewEntered.get_future().wait_for(2s) == std::future_status::ready &&
        run.recordEntered.get_future().wait_for(2s) == std::future_status::ready);
  CHECK(run.send(1) && run.send(2));
  auto full = run.source.emit(capture(3));
  CHECK(full.retryable() && full.accepted == 0);
  run.unblockRecord.set_value();
  for (int i = 3; i < frames - 1; ++i)
    CHECK(run.send(i));
  run.unblockPreview.set_value();
  std::error_code ec;
  CHECK(!std::filesystem::exists(path, ec) && !ec);
  // The capture node publishes the final frame from its stop method.
  auto stopped = graph.stop(Clock::now() + 5s, gf::StopMode::Drain);
  CHECK(stopped.error.code != gf::ErrorCode::Timeout);
  CHECK(run.tail && run.written == frames && run.evicted > 0 && run.shown + run.evicted == frames);
  if (finalizationFails) {
    CHECK(!stopped && stopped.error.phase == gf::Phase::Stop &&
          stopped.error.node == "file.record" && run.finishError &&
          stopped.error.detail == run.finishError);
    CHECK(!std::filesystem::exists(path, ec) && !ec);
  } else {
    CHECK(stopped && std::filesystem::file_size(path, ec) > 0 && !ec);
  }
  CHECK(graph.close());
  return true;
}

// Fail an actual buffered MKV write, not just the later MP4 open/rename.
// Drain after each admitted event to distinguish admission from encoder failure.
bool lockedRecording(const std::filesystem::path& root) {
  Run run;
  run.unblockRecord.set_value();
  auto mp4 = root / "locked.mp4";
  auto mkv = root / "locked.mkv";
  gf::Graph graph({gf::Dispatch::Queued, 2});
  auto src = graph.ingress<Capture>("capture");
  auto rec = graph.add<Record>("file.record", run, mp4);
  CHECK(src && rec && graph.rewire([&](auto& w) {
    w.connect(src->port, rec->in(gf::In<Capture>{"av"}), {}, {gf::DeliveryPolicy::Reliable, 2});
  }) && graph.start());
  auto send = [&](int n) {
    auto r = src->writer.emit(capture(n));
    return r.status == gf::Admission::Accepted && r.accepted == 1;
  };
  // Leave complete clusters on disk before making the next write fail.
  int n = 0;
  for (; n < 160; ++n)
    CHECK(send(n) && graph.drain(Clock::now() + 2s));
  std::error_code ec;

  struct Handle {
    HANDLE h;

    ~Handle() {
      if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    }
  };

  gf::Status delivery;
  {
    Handle handle{CreateFileW(mkv.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, 0, nullptr)};
    CHECK(handle.h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER committed{};
    do {
      CHECK(GetFileSizeEx(handle.h, &committed));
      if (committed.QuadPart >= 64 * 1024)
        break;
      CHECK(n < 3000 && send(n++) && graph.drain(Clock::now() + 2s));
    } while (true);
    const int end = n + 2000;
    OVERLAPPED range{};
    CHECK(LockFileEx(handle.h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, 0,
                     &range));
    for (; n < end && delivery; ++n) {
      CHECK(send(n));
      delivery = graph.drain(Clock::now() + 2s);
    }
    CHECK(!delivery && delivery.error.code == gf::ErrorCode::NodeFailure &&
          delivery.error.phase == gf::Phase::Drain && delivery.error.node == "file.record" &&
          delivery.error.port == "av");
    CHECK(UnlockFileEx(handle.h, 0, MAXDWORD, 0, &range));
  }
  auto stopped = graph.stop(Clock::now() + 2s);
  CHECK(!stopped && stopped.error.phase == gf::Phase::Stop && run.finishError &&
        run.finishError->backend_reason < 0 && delivery.error.detail == run.finishError &&
        stopped.error.detail == run.finishError);
  CHECK(graph.close() && !std::filesystem::exists(mp4, ec) && !ec);
  Handle exclusive{CreateFileW(mkv.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
  CHECK(exclusive.h != INVALID_HANDLE_VALUE);
  std::ofstream evidence(root / "locked-write.txt");
  evidence << run.written << ' ' << run.finishError->backend_reason << '\n';
  evidence.close();
  return bool(evidence);
}

// Exercise standalone encoding and idempotent finalization without a graph.
bool standaloneRecorder(const std::filesystem::path& path) {
  Recorder rec(path, width, height, 25);
  CHECK(rec.status());
  auto first = capture(0);
  auto now = Clock::now();
  CHECK(rec.video(first->image, now) && rec.audio(first->audio, now));
  CHECK(rec.finish() && rec.finish());
  return true;
}
}  // namespace

int mediaProfile(const std::filesystem::path&, int);

int mediaCapacity(const std::filesystem::path&, int frames, int sessions, int rounds);

static int number(std::string_view text) {
  int value = 0;
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? value : -1;
}

int main(int argc, char** argv) {
  if (argc == 4 && std::string_view(argv[1]) == "--profile")
    return mediaProfile(std::filesystem::u8path(argv[2]), number(argv[3]));
  if (argc == 6 && std::string_view(argv[1]) == "--capacity")
    return mediaCapacity(std::filesystem::u8path(argv[2]), number(argv[3]), number(argv[4]),
                         number(argv[5]));
  if (argc != 2)
    return 2;
  std::filesystem::path root = std::filesystem::u8path(argv[1]);
  return recordFile(root / "record.mp4", false) && recordFile(root / "failed.mp4", true) &&
                 lockedRecording(root) && standaloneRecorder(root / "standalone.mp4")
             ? 0
             : 1;
}
