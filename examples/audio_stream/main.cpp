#include "../media_result.hpp"
using livekit::sdk::examples::report_result;
#include <livekit/media_engine.hpp>
#include <map>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>

using namespace livekit::sdk::client;
using namespace std::chrono_literals;

namespace {
struct ReceivedAudio {
  TrackId track;
  AudioSample frame;
};

class ApplicationQueue {
 public:
  void push(const TrackId& track, const AudioSample& frame) {
    if (!frame.pcm) {
      return;
    }
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
  MediaEngine engine;
  std::map<TrackSid, std::shared_ptr<MediaTrack>> subscriptions;
  ConnectionOptions options;
  options.auto_subscribe = false;
  options.url = url;
  options.token = token;
  if (!engine.connect(options)) {
    return 1;
  }
  std::map<TrackId, uint64_t> samples;
  size_t blocks = 0;
  const auto deadline = SteadyClock::now() + 30s;
  while (blocks < 100 && SteadyClock::now() < deadline) {
    for (const auto& peer : engine.remote_participants()) {
      for (const auto& publication : peer.track_publications()) {
        const auto info = publication.snapshot();
        if (info.kind != MediaKind::audio || subscriptions.count(info.sid)) {
          continue;
        }
        MediaSubscribeSpec config;
        config.publication = publication;
        config.output.playout = playout;
        config.output.audio = [&queue, id = info.sid](const auto& frame) {
          queue.push(id, frame);
          return livekit::sdk::Result<void>::success();
        };
        auto operation_result = engine.subscribe_media(std::move(config));
        if (!report_result(operation_result)) {
          return 1;
        }
        subscriptions[info.sid] = operation_result->track;
      }
    }

    for (auto it = subscriptions.begin(); it != subscriptions.end();) {
      auto state = it->second->snapshot();
      if (state.failure) {
        std::cerr << state.failure->message << '\n';
        return 1;
      }
      if (state.transport.state == TrackState::closed) {
        engine.unsubscribe_track(it->first);
        it = subscriptions.erase(it);
      } else {
        ++it;
      }
    }
    if (engine.connection_state() == ConnectionState::failed) {
      return 1;
    }
    std::this_thread::sleep_for(10ms);
    // Slow application work happens here, outside decoder/native callbacks.
    // A playout consumer must also track epoch, timestamp gaps and discontinuity;
    // receiving PCM here does not provide a device clock or jitter buffering.
    for (const auto& received : queue.take()) {
      samples[received.track] += received.frame.pcm->samples_per_channel();
      ++blocks;
    }
  }
  engine.close();
  for (const auto& [track, count] : samples) {
    std::cout << track << " samples/channel=" << count << '\n';
  }
  std::cout << "application_dropped_blocks=" << queue.dropped << '\n';
  return blocks ? 0 : 1;
}
