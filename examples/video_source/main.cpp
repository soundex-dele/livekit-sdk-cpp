#include "../media_result.hpp"
using livekit::sdk::examples::report_result;
#include <livekit/media_engine.hpp>
#include <algorithm>
#include <iostream>
#include <thread>
using namespace livekit::sdk::client;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: video_source <server-url> <token>\n";
    return 2;
  }

  MediaEngine engine;
  ConnectionOptions options;
  options.url = argv[1];
  options.token = argv[2];
  if (!engine.connect(options)) {
    return 1;
  }
  MediaPublishSpec config;
  config.name = "application-video";
  config.format.codec = Codec::vp8;
  config.format.width = 640;
  config.format.height = 360;
  config.input.generated = true;
  auto operation_result = engine.publish_media(std::move(config));
  if (!report_result(operation_result)) {
    return 1;
  }
  auto operation = *std::move(operation_result);
  const auto id = operation.publication.snapshot().track_id;
  const auto end = SteadyClock::now() + 10s;
  while (SteadyClock::now() < end) {
    if (engine.connection_state() == ConnectionState::failed ||
        engine.connection_state() == ConnectionState::closed) {
      return 1;
    }
    if (auto failure = operation.track->snapshot().failure) {
      std::cerr << failure->message << '\n';
      return 1;
    }
    std::this_thread::sleep_for(10ms);
  }
  engine.unpublish_track(id);
  engine.close();
  std::cout << "Submitted encoded access units: " << operation.track->snapshot().frames << '\n';
}
