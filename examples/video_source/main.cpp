#include "rtc_blocking_bridge.hpp"
#include "../media_result.hpp"
using livekit::sdk::examples::report_result;
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

  auto engine_result =
      livekit::sdk::app_support::create_blocking_engine(livekit::sdk::rtc::AudioOutputOptions{});
  if (!report_result(engine_result)) {
    return 1;
  }
  auto engine = *std::move(engine_result);
  livekit::sdk::app_support::RtcEventMailbox engine_events(*engine);
  if (!report_result(engine_events.status())) {
    return 1;
  }
  LiveKitRoomOptions options;
  options.url = argv[1];
  options.token = argv[2];
  if (!report_result(engine->join(options))) {
    return 1;
  }
  TrackDescriptor descriptor;
  descriptor.id = descriptor.name = "application-video";
  descriptor.format.codec = Codec::vp8;
  descriptor.format.width = 640;
  descriptor.format.height = 360;
  engine::MediaPublishSpec config;
  config.track = descriptor;
  config.input.generated = true;
  auto operation_result = engine->publish_video(std::move(config));
  if (!report_result(operation_result)) {
    return 1;
  }
  auto operation = *std::move(operation_result);
  const auto id = operation.id;
  const auto end = SteadyClock::now() + 10s;
  while (SteadyClock::now() < end) {
    auto failure_result = engine_events.take_failure();
    if (!report_result(failure_result)) {
      return 1;
    }
    if (auto failure = *std::move(failure_result)) {
      std::cerr << failure->message << "\n";
      return 1;
    }
    std::this_thread::sleep_for(10ms);
  }
  if (!report_result(engine->unpublish(id))) {
    return 1;
  }
  if (!report_result(engine->close())) {
    return 1;
  }
  const auto stats = *operation.track->snapshot().video;
  std::cout << "Encoded frames: " << stats.completed << ", dropped: " << stats.dropped << '\n';
}
