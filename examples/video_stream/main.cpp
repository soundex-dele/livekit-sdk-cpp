#include "rtc_blocking_bridge.hpp"
#include "../media_result.hpp"
using livekit::sdk::examples::report_result;
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
#include <livekit/rtc/engine.hpp>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

int main() {
  const auto url = std::getenv("LIVEKIT_URL"), token = std::getenv("LIVEKIT_TOKEN");
  if (!url || !token) {
    std::cerr << "Set LIVEKIT_URL and a room-join LIVEKIT_TOKEN" << "\n";
    return 1;
  }
  std::atomic<size_t> received{0};
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
  options.url = url;
  options.token = token;
  if (!report_result(engine->join(options))) {
    return 1;
  }
  const auto deadline = SteadyClock::now() + 30s;
  while (received < 50 && SteadyClock::now() < deadline) {
    auto event_result = engine_events.take_event(10ms);
    if (!report_result(event_result)) {
      return 1;
    }
    if (auto event = *std::move(event_result); event &&
                                               event->kind == RoomEventKind::remote_track_added &&
                                               event->descriptor->kind == MediaKind::video) {
      auto descriptor = *event->descriptor;
      engine::MediaSubscribeSpec config;
      config.track = descriptor;
      config.output.video = [&received](const auto& event) {
        if (!event.pixels || event.pixels->y.empty()) {
          return livekit::sdk::Result<void>::failure(livekit::sdk::make_failure(
              livekit::sdk::ErrorDomain::media, livekit::sdk::ErrorCode::processing_failed,
              "Empty decoded video"));
        }
        ++received;
        return livekit::sdk::Result<void>::success();
      };
      auto operation_result = engine->subscribe_media(std::move(config));
      if (!report_result(operation_result)) {
        return 1;
      }
      auto operation = *std::move(operation_result);
    }

    auto failure_result = engine_events.take_failure();
    if (!report_result(failure_result)) {
      return 1;
    }
    if (auto failure = *std::move(failure_result); failure && !failure->message.empty()) {
      std::cerr << failure->message << "\n";
      return 1;
    }
  }
  if (!report_result(engine->close())) {
    return 1;
  }
  std::cout << "frames=" << received << '\n';
  return received ? 0 : 1;
}
