#include <livekit/rtc/media_engine.hpp>
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
  try {
    if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--playout"))
      throw std::invalid_argument("Usage: audio_stream [--playout]");
    const bool playout = argc == 2;
    const auto url = std::getenv("LIVEKIT_URL"), token = std::getenv("LIVEKIT_TOKEN");
    if (!url || !token)
      throw std::runtime_error("Set LIVEKIT_URL and a room-join LIVEKIT_TOKEN");
    ApplicationQueue queue;  // Outlives the engine and its decoder workers.
    auto engine = engine::create_livekit_media_engine();
    RoomOptions options;
    options.url = url;
    options.token = token;
    engine->join(options).get();
    std::vector<std::future<TrackId>> pending;
    std::map<TrackId, uint64_t> samples;
    size_t blocks = 0;
    const auto deadline = SteadyClock::now() + 30s;
    while (blocks < 100 && SteadyClock::now() < deadline) {
      if (auto event = engine->room().next_event(10ms);
          event && event->kind == RoomEventKind::remote_track_added && event->descriptor &&
          event->descriptor->kind == MediaKind::audio) {
        const auto descriptor = *event->descriptor;
        engine::MediaSubscribeSpec config;
        config.track = descriptor;
        config.output.playout = playout;
        config.output.audio = [&queue, id = descriptor.id](const auto& frame) {
          queue.push(id, frame);
        };
        auto operation = engine->subscribe_media(std::move(config));
        pending.push_back(std::move(operation.ready));
      }
      engine->poll();
      for (auto it = pending.begin(); it != pending.end();) {
        if (it->wait_for(0ms) == std::future_status::ready) {
          it->get();
          it = pending.erase(it);
        } else
          ++it;
      }
      if (auto failure = engine->next_failure())
        throw std::runtime_error(failure->message);
      // Slow application work happens here, outside decoder/native callbacks.
      // A playout consumer must also track epoch, timestamp gaps and discontinuity;
      // receiving PCM here does not provide a device clock or jitter buffering.
      for (const auto& received : queue.take()) {
        samples[received.track] += received.frame.pcm->samples_per_channel();
        ++blocks;
      }
    }
    engine->close().get();
    for (const auto& [track, count] : samples)
      std::cout << track << " samples/channel=" << count << '\n';
    std::cout << "application_dropped_blocks=" << queue.dropped << '\n';
    return blocks ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
