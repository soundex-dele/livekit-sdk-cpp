#include "rtc_blocking_bridge.hpp"
#include "../../support/rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
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
    auto engine =
        require_result(livekit::sdk::app_support::create_blocking_engine(livekit::sdk::rtc::AudioOutputOptions{}));
    livekit::sdk::app_support::RtcEventMailbox engine_events(*engine);
    LiveKitRoomOptions options;
    options.url = argv[1];
    options.token = argv[2];
    require_result(engine->join(options));
    TrackDescriptor descriptor;
    descriptor.id = descriptor.name = "application-video";
    descriptor.format.codec = Codec::vp8;
    descriptor.format.width = 640;
    descriptor.format.height = 360;
    engine::MediaPublishSpec config;
    config.track = descriptor;
    config.input.generated = true;
    auto operation = require_result(engine->publish_video(std::move(config)));
    const auto id = operation.id;
    const auto end = SteadyClock::now() + 10s;
    while (SteadyClock::now() < end) {
      if (auto failure = engine_events.take_failure())
        throw std::runtime_error(failure->message);
      std::this_thread::sleep_for(10ms);
    }
    require_result(engine->unpublish(id));
    require_result(engine->close());
    const auto stats = *operation.track->snapshot().video;
    std::cout << "Encoded frames: " << stats.completed << ", dropped: " << stats.dropped << '\n';
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
