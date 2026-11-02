#include "rtc_blocking_bridge.hpp"
#include "../media_result.hpp"
using livekit::sdk::examples::report_result;
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
#include <livekit/rtc/engine.hpp>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>

using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

namespace {
struct ReceivedAudio {
  TrackId track;
  engine::AudioSample frame;
};

class ApplicationQueue {
 public:
  void push(const TrackId& track, const engine::AudioSample& frame) {
    if (!frame.pcm)
      return;
    const auto bytes = frame.pcm->samples.size() * sizeof(int16_t);
    // Never wait for application processing on the decoder worker.
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock || pending_.size() >= 128 || bytes > 512 * 1024 - bytes_) {
      ++dropped;
      return;
    }
    pending_.push_back({track, frame});
    bytes_ += bytes;
  }

  std::deque<ReceivedAudio> take() {
    std::lock_guard lock(mutex_);
    std::deque<ReceivedAudio> batch;
    batch.swap(pending_);
    bytes_ = 0;
    return batch;
  }

  std::atomic<uint64_t> dropped{0};

 private:
  std::mutex mutex_;
  std::deque<ReceivedAudio> pending_;
  size_t bytes_ = 0;
};
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--playout")) {
    std::cerr << "Usage: audio_stream [--playout]" << "\n";
    return 1;
  }
  const bool playout = argc == 2;
  const auto url = std::getenv("LIVEKIT_URL"), token = std::getenv("LIVEKIT_TOKEN");
  if (!url || !token) {
    std::cerr << "Set LIVEKIT_URL and a room-join LIVEKIT_TOKEN" << "\n";
    return 1;
  }
  ApplicationQueue queue;  // Outlives the engine and its decoder workers.
  auto engine_result =
      livekit::sdk::app_support::create_blocking_engine(livekit::sdk::rtc::AudioOutputOptions{});
  if (!report_result(engine_result)) {
    return 1;
  }
  auto engine = *std::move(engine_result);
  livekit::sdk::app_support::RtcEventMailbox engine_events(*engine);
  if (!report_result(engine_events.status())) {
    return 1;
  }
  LiveKitRoomOptions options;
  options.url = url;
  options.token = token;
  if (!report_result(engine->join(options))) {
    return 1;
  }
  std::map<TrackId, uint64_t> samples;
  size_t blocks = 0;
  const auto deadline = SteadyClock::now() + 30s;
  while (blocks < 100 && SteadyClock::now() < deadline) {
    auto event_result = engine_events.take_event(10ms);
    if (!report_result(event_result)) {
      return 1;
    }
    if (auto event = *std::move(event_result);
        event && event->kind == RoomEventKind::remote_track_added && event->descriptor &&
        event->descriptor->kind == MediaKind::audio) {
      const auto descriptor = *event->descriptor;
      engine::MediaSubscribeSpec config;
      config.track = descriptor;
      config.output.playout = playout;
      config.output.audio = [&queue, id = descriptor.id](const auto& frame) {
        queue.push(id, frame);
        return livekit::sdk::Result<void>::success();
      };
      auto operation_result = engine->subscribe_media(std::move(config));
      if (!report_result(operation_result)) {
        return 1;
      }
      auto operation = *std::move(operation_result);
    }

    auto failure_result = engine_events.take_failure();
    if (!report_result(failure_result)) {
      return 1;
    }
    if (auto failure = *std::move(failure_result)) {
      std::cerr << failure->message << "\n";
      return 1;
    }
    // Slow application work happens here, outside decoder/native callbacks.
    // A playout consumer must also track epoch, timestamp gaps and discontinuity;
    // receiving PCM here does not provide a device clock or jitter buffering.
    for (const auto& received : queue.take()) {
      samples[received.track] += received.frame.pcm->samples_per_channel();
      ++blocks;
    }
  }
  if (!report_result(engine->close())) {
    return 1;
  }
  for (const auto& [track, count] : samples) {
    std::cout << track << " samples/channel=" << count << '\n';
  }
  std::cout << "application_dropped_blocks=" << queue.dropped << '\n';
  return blocks ? 0 : 1;
}
