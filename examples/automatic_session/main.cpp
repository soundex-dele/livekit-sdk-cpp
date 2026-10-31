#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/livekit.hpp"
#include <iostream>
#include <thread>
using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::engine;
using namespace std::chrono_literals;

namespace {
void report(const FailureInfo& failure) {
  std::cerr << failure.message << '\n';
}

struct Observer final : RtcObserver {
  RtcEngine* engine = nullptr;

  void on_room_event(const RoomEvent& event) override {
    if (event.kind == RoomEventKind::remote_track_added && event.descriptor) {
      MediaSubscribeSpec spec;
      spec.track = *event.descriptor;
      spec.output.playout = true;
      engine->subscribe_media(std::move(spec), [](Result<MediaSubscription> result) {
        if (!result) {
          report(result.error());
        }
      });
    }
  }
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: automatic_session <url> <token>\n";
    return 2;
  }
  Observer observer;
  auto created = create_livekit_engine(AudioOutputOptions{});
  if (!created) {
    report(created.error());
    return 1;
  }
  auto engine = *std::move(created);
  observer.engine = engine.get();
  if (engine->set_observer(&observer) != ErrorCode::ok) {
    return 1;
  }
  std::shared_ptr<MediaTrack> publication;
  LiveKitRoomOptions options;
  options.url = argv[1];
  options.token = argv[2];
  engine->join(options, [&](Result<RoomSnapshot> joined) {
    if (!joined) {
      report(joined.error());
      return;
    }
    MediaPublishSpec spec;
    spec.track.id = "generated-audio";
    spec.track.kind = MediaKind::audio;
    spec.track.format.codec = Codec::opus;
    spec.track.format.clock_rate = 48000;
    spec.track.format.channels = 1;
    spec.input.generated = true;
    engine->publish_audio(std::move(spec), [&](Result<MediaPublication> result) {
      if (!result) {
        report(result.error());
        return;
      }
      publication = result->track;
    });
  });
  std::this_thread::sleep_for(10s);
  engine->leave();
  if (publication) {
    std::cout << "published frames: " << publication->snapshot().frames << '\n';
  }
}
