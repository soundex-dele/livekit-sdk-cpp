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

// Independent application observers. Promises are this console application's
// waiting mechanism; engine operations themselves take no completion callable.
struct RoomEvents final : RoomObserver {
  std::promise<OperationResult> joined, closed;

  void on_join_completed(const OperationResult& result) override {
    joined.set_value(result);
  }

  void on_close_completed(const OperationResult& result) override {
    closed.set_value(result);
  }

  void on_room_event(const RoomEvent& event) override {
    if (event.kind == RoomEventKind::failed) {
      std::cerr << "Room failed: " << event.message << '\n';
    }
  }
};

struct PublicationEvents final : livekit::sdk::rtc::engine::PublicationObserver {
  std::promise<Result<MediaPublication>> published;

  void on_media_publish_completed(const TrackId&, Result<MediaPublication> result) override {
    published.set_value(std::move(result));
  }

  void on_graph_failure(const GraphFailure& failure) override {
    std::cerr << failure.id << ": " << failure.message << '\n';
  }
};

void check_completed(OperationResult result) {
  if (result.failure) {
    throw SDKError(std::move(result.failure));
  }
  require_result(result.code);
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: automatic_session <url> <token>\n";
    return 2;
  }
  try {
    auto room = std::make_shared<RoomEvents>();
    auto publications = std::make_shared<PublicationEvents>();
    auto engine = require_result(create_livekit_engine(AudioOutputOptions{}));
    engine->set_room_observer(room);
    engine->set_publication_observer(publications);
    LiveKitRoomOptions options;
    options.url = argv[1];
    options.token = argv[2];
    require_result(engine->join(options));  // Admission only.
    check_completed(room->joined.get_future().get());
    MediaPublishSpec spec;
    spec.track.id = "generated-audio";
    spec.track.kind = MediaKind::audio;
    spec.track.format.codec = Codec::opus;
    spec.track.format.clock_rate = 48000;
    spec.track.format.channels = 1;
    spec.input.generated = true;
    require_result(engine->publish_audio(spec));
    auto publication = require_result(publications->published.get_future().get());
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::cout << "published frames: " << publication.track->snapshot().frames << '\n';
    require_result(engine->close());
    check_completed(room->closed.get_future().get());
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
