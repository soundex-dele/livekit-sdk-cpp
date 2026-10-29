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
  try {
    const auto url = std::getenv("LIVEKIT_URL"), token = std::getenv("LIVEKIT_TOKEN");
    if (!url || !token)
      throw std::runtime_error("Set LIVEKIT_URL and a room-join LIVEKIT_TOKEN");
    std::atomic<size_t> received{0};
    auto engine = engine::create_livekit_engine({});
    livekit::sdk::app_support::RtcEventMailbox engine_events(*engine);
    LiveKitRoomOptions options;
    options.url = url;
    options.token = token;
    engine->join(options).get();
    std::vector<livekit::sdk::AsyncResult<TrackId>> pending;
    const auto deadline = SteadyClock::now() + 30s;
    while (received < 50 && SteadyClock::now() < deadline) {
      if (auto event = engine_events.take_event(10ms);
          event && event->kind == RoomEventKind::remote_track_added &&
          event->descriptor->kind == MediaKind::video) {
        auto descriptor = *event->descriptor;
        engine::MediaSubscribeSpec config;
        config.track = descriptor;
        config.output.video = [&received](const auto& event) {
          if (!event.pixels || event.pixels->y.empty())
            throw std::runtime_error("Empty decoded video");
          ++received;
        };
        auto operation = engine->subscribe_media(std::move(config));
        pending.push_back(std::move(operation.ready));
      }

      for (auto it = pending.begin(); it != pending.end();) {
        if (it->wait_for(0ms) == std::future_status::ready) {
          it->get();
          it = pending.erase(it);
        } else
          ++it;
      }
      if (auto failure = engine_events.take_failure(); failure && !failure->message.empty())
        throw std::runtime_error(failure->message);
    }
    engine->close().get();
    std::cout << "frames=" << received << '\n';
    return received ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
