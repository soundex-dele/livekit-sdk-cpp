#include "rtc_result.hpp"
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/livekit.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::engine;
using livekit::sdk::app_support::require_result;

// Observers receive ongoing events. Per-call completions resolve this console
// application's promises; only its main thread waits, never an SDK callback.
struct RoomEvents final : RoomObserver {
  void on_room_event(const RoomEvent& event) override {
    if (event.kind == RoomEventKind::failed) {
      std::cerr << "Room failed: " << event.message << '\n';
    }
  }
};

struct PublicationEvents final : livekit::sdk::rtc::engine::PublicationObserver {
  void on_graph_failure(const GraphFailure& failure) override {
    std::cerr << failure.id << ": " << failure.message << '\n';
  }
};

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: automatic_session <url> <token>\n";
    return 2;
  }
  try {
    auto room = std::make_shared<RoomEvents>();
    auto publications = std::make_shared<PublicationEvents>();
    // These targets outlive engine destruction, which drains outstanding callbacks.
    std::promise<Result<void>> joined, closed;
    std::promise<Result<MediaPublication>> published;
    auto engine = require_result(create_livekit_engine(AudioOutputOptions{}));
    engine->set_room_observer(room);
    engine->set_publication_observer(publications);
    LiveKitRoomOptions options;
    options.url = argv[1];
    options.token = argv[2];
    require_result(engine->join(options, [&](Result<void> result) {
      joined.set_value(std::move(result));
    }));  // Admission only.
    require_result(joined.get_future().get());
    MediaPublishSpec spec;
    spec.track.id = "generated-audio";
    spec.track.kind = MediaKind::audio;
    spec.track.format.codec = Codec::opus;
    spec.track.format.clock_rate = 48000;
    spec.track.format.channels = 1;
    spec.input.generated = true;
    require_result(engine->publish_audio(
        spec, [&](Result<MediaPublication> result) { published.set_value(std::move(result)); }));
    auto publication = require_result(published.get_future().get());
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::cout << "published frames: " << publication.track->snapshot().frames << '\n';
    require_result(
        engine->close([&](Result<void> result) { closed.set_value(std::move(result)); }));
    require_result(closed.get_future().get());
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
