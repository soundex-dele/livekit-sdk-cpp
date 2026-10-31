#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/livekit.hpp"
#include <iostream>
#include <thread>
using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::engine;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: automatic_session <url> <token>\n";
    return 2;
  }
  auto engine = create_livekit_engine(AudioOutputOptions{});
  std::shared_ptr<MediaTrack> publication;
  RtcCallbacks callbacks;
  callbacks.on_joined = [&](const RoomSnapshot&) {
    MediaPublishSpec spec;
    spec.track.id = "generated-audio";
    spec.track.kind = MediaKind::audio;
    spec.track.format.codec = Codec::opus;
    spec.track.format.clock_rate = 48000;
    spec.track.format.channels = 1;
    spec.input.generated = true;
    engine->publish_audio(std::move(spec));
  };
  callbacks.on_published = [&](const MediaPublication& value) { publication = value.track; };
  callbacks.on_track_published = [&](const TrackDescriptor& track) {
    MediaSubscribeSpec spec;
    spec.track = track;
    spec.output.playout = true;
    engine->subscribe_media(std::move(spec));
  };
  engine->set_callbacks(std::move(callbacks));
  LiveKitRoomOptions options;
  options.url = argv[1];
  options.token = argv[2];
  engine->join(options);
  std::this_thread::sleep_for(10s);
  engine->leave();
  if (publication) {
    std::cout << "published frames: " << publication->snapshot().frames << '\n';
  }
}
