#include <livekit/rtc/engine.hpp>
#include <algorithm>
#include <iostream>
#include <thread>
using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: video_source <server-url> <token>\n";
    return 2;
  }
  try {
    auto engine = engine::create_livekit_engine({});
    RoomOptions options;
    options.url = argv[1];
    options.token = argv[2];
    engine->join(options).get();
    TrackDescriptor descriptor;
    descriptor.id = descriptor.name = "application-video";
    descriptor.format.codec = Codec::vp8;
    descriptor.format.width = 640;
    descriptor.format.height = 360;
    engine::MediaPublishSpec config;
    config.track = descriptor;
    config.input.generated = true;
    auto operation = engine->publish_video(std::move(config));
    auto publication = std::move(operation.ready);
    while (publication.wait_for(0ms) != std::future_status::ready) {
      engine->poll();
      std::this_thread::sleep_for(1ms);
    }
    const auto id = publication.get();
    const auto end = SteadyClock::now() + 10s;
    while (SteadyClock::now() < end) {
      engine->poll();
      if (auto failure = engine->next_failure())
        throw std::runtime_error(failure->message);
      std::this_thread::sleep_for(10ms);
    }
    engine->unpublish(id).get();
    engine->close().get();
    const auto stats = *operation.track->snapshot().video;
    std::cout << "Encoded frames: " << stats.completed << ", dropped: " << stats.dropped << '\n';
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
