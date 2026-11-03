#include "livekit/media_engine.hpp"
#include <iostream>
#include <map>
#include <thread>
using namespace livekit::sdk::client;
using namespace std::chrono_literals;

struct Observer final : RoomObserver {
  void on_room_event(const RoomEvent& event) override {
    if (event.failure) {
      std::cerr << event.failure->message << '\n';
    }
    // Synchronous controls belong on the application thread, outside this callback.
  }
};

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: automatic_session <url> <token>\n";
    return 2;
  }
  Observer observer;
  MediaEngine engine;
  engine.set_observer(&observer);
  ConnectionOptions options;
  options.url = argv[1];
  options.token = argv[2];
  options.auto_subscribe = false;
  if (!engine.connect(options)) {
    return 1;
  }
  MediaPublishSpec spec;
  spec.name = "generated-audio";
  spec.kind = MediaKind::audio;
  spec.format = {Codec::opus, 48000, 1};
  spec.input.generated = true;
  auto publication = engine.publish_media(std::move(spec));
  if (!publication) {
    std::cerr << publication.error().message << '\n';
    return 1;
  }
  std::map<TrackSid, std::shared_ptr<MediaTrack>> subscriptions;
  const auto end = SteadyClock::now() + 10s;
  while (SteadyClock::now() < end) {
    if (engine.connection_state() == ConnectionState::failed ||
        engine.connection_state() == ConnectionState::closed) {
      return 1;
    }
    for (const auto& peer : engine.remote_participants()) {
      for (const auto& remote : peer.track_publications()) {
        auto info = remote.snapshot();
        if (subscriptions.count(info.sid)) {
          continue;
        }
        MediaSubscribeSpec receive;
        receive.publication = remote;
        receive.output.playout = info.kind == MediaKind::audio;
        auto result = engine.subscribe_media(std::move(receive));
        if (!result) {
          if (remote.snapshot().state == TrackState::closed) {
            continue;
          }
          std::cerr << result.error().message << '\n';
          return 1;
        }
        subscriptions.emplace(info.sid, result->track);
      }
    }
    for (auto it = subscriptions.begin(); it != subscriptions.end();) {
      const auto snapshot = it->second->snapshot();
      if (snapshot.failure) {
        std::cerr << snapshot.failure->message << '\n';
        return 1;
      }
      if (snapshot.transport.state == TrackState::closed) {
        engine.unsubscribe_track(it->first);
        it = subscriptions.erase(it);
      } else {
        ++it;
      }
    }
    if (auto failure = publication->track->snapshot().failure) {
      std::cerr << failure->message << '\n';
      return 1;
    }
    std::this_thread::sleep_for(20ms);
  }
  engine.close();
  std::cout << "published frames: " << publication->track->snapshot().frames << '\n';
}
