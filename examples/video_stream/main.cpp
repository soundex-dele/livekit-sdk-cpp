#include "../media_result.hpp"
using livekit::sdk::examples::report_result;
#include <livekit/media_engine.hpp>
#include <map>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <iostream>
using namespace livekit::sdk::client;
using namespace std::chrono_literals;

int main() {
  const auto url = std::getenv("LIVEKIT_URL"), token = std::getenv("LIVEKIT_TOKEN");
  if (!url || !token) {
    std::cerr << "Set LIVEKIT_URL and a room-join LIVEKIT_TOKEN" << "\n";
    return 1;
  }
  std::atomic<size_t> received{0};
  MediaEngine engine;
  std::map<TrackSid, std::shared_ptr<MediaTrack>> subscriptions;
  ConnectionOptions options;
  options.auto_subscribe = false;
  options.url = url;
  options.token = token;
  if (!engine.connect(options)) {
    return 1;
  }
  const auto deadline = SteadyClock::now() + 30s;
  while (received < 50 && SteadyClock::now() < deadline) {
    for (const auto& peer : engine.remote_participants()) {
      for (const auto& publication : peer.track_publications()) {
        const auto info = publication.snapshot();
        if (info.kind != MediaKind::video || subscriptions.count(info.sid)) {
          continue;
        }
        MediaSubscribeSpec config;
        config.publication = publication;
        config.output.video = [&received](const auto& event) {
          if (!event.pixels || event.pixels->y.empty()) {
            return livekit::sdk::Result<void>::failure(livekit::sdk::make_failure(
                livekit::sdk::ErrorDomain::media, livekit::sdk::ErrorCode::processing_failed,
                "Empty decoded video"));
          }
          ++received;
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
  }
  engine.close();
  std::cout << "frames=" << received << '\n';
  return received ? 0 : 1;
}
