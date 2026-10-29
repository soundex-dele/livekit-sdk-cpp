#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/livekit.hpp"
#include <chrono>
#include <iostream>
#include <thread>

using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::engine;

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: automatic_session <url> <token>\n";
    return 2;
  }
  try {
    auto engine = create_livekit_engine(AudioOutputOptions{});
    engine->set_callbacks({.on_event =
                               [](const RoomEvent& event) {
                                 if (event.kind == RoomEventKind::failed) {
                                   std::cerr << "Room failed: " << event.message << '\n';
                                 }
                               },
                           .on_failure =
                               [](const RtcEngine::GraphFailure& failure) {
                                 std::cerr << failure.id << ": " << failure.message << '\n';
                               }});
    LiveKitRoomOptions options;
    options.url = argv[1];
    options.token = argv[2];
    engine->join(options).get();
    MediaPublishSpec spec;
    spec.track.id = "generated-audio";
    spec.track.kind = MediaKind::audio;
    spec.track.format.codec = Codec::opus;
    spec.track.format.clock_rate = 48000;
    spec.track.format.channels = 1;
    spec.input.generated = true;
    auto operation = engine->publish_audio(spec);
    operation.ready.get();  // Backend completion schedules graph startup automatically.
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const auto snapshot = operation.track->snapshot();
    std::cout << "published frames: " << snapshot.frames << '\n';
    engine->close().get();
    engine->set_callbacks({});
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
