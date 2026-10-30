#include "rtc_blocking_bridge.hpp"
#include "rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/livekit.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::engine;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <class Future>
auto await(Future future) {
  return require_result(std::move(future));
}

struct Observation {
  uint64_t frames = 0;
  bool ten_ms_audio = false;
  std::optional<livekit::sdk::media::FrameView> retained;
};
}  // namespace

// Isolated-SFU integration using only the ordinary public API. Generated media
// and application callbacks avoid opening any physical capture/playout device.
int main() {
  try {
    std::mutex mutex;
    std::array<Observation, 2> observed;
    auto tx = require_result(livekit::sdk::app_support::create_blocking_engine(AudioOutputOptions{}));
    auto rx = require_result(livekit::sdk::app_support::create_blocking_engine(AudioOutputOptions{}));
    livekit::sdk::app_support::RtcEventMailbox rx_events;
    rx_events.attach(*rx);
    LiveKitRoomOptions options;
    options.url = std::getenv("LIVEKIT_URL");
    options.token = std::getenv("LIVEKIT_PUBLISH_TOKEN");
    options.reconnect.enabled = false;
    await(tx->join(options));
    options.token = std::getenv("LIVEKIT_SUBSCRIBE_TOKEN");
    await(rx->join(options));
    std::array<std::shared_ptr<MediaTrack>, 2> publications, subscriptions;
    for (size_t i = 0; i < 2; ++i) {
      MediaPublishSpec spec;
      spec.track.id = spec.track.name = i ? "ordinary-video" : "ordinary-audio";
      spec.track.stream_group = "av-sync";
      spec.track.kind = i ? MediaKind::video : MediaKind::audio;
      spec.track.format.codec = i ? Codec::vp8 : Codec::opus;
      spec.track.format.clock_rate = i ? 90000 : 48000;
      spec.track.format.channels = 2;
      spec.track.format.width = 64;
      spec.track.format.height = 48;
      spec.input.generated = true;
      auto operation = await(tx->publish_media(spec));
      publications[i] = operation.track;
    }
    const auto deadline = SteadyClock::now() + 20s;
    while (!subscriptions[0] || !subscriptions[1]) {
      check(SteadyClock::now() < deadline, "Ordinary media discovery timed out");
      auto event = rx_events.take_event(20ms);
      if (!event || event->kind != RoomEventKind::remote_track_added) {
        std::this_thread::sleep_for(5ms);
        continue;
      }
      const size_t i = event->descriptor->kind == MediaKind::video ? 1 : 0;
      if (subscriptions[i]) {
        continue;
      }
      MediaSubscribeSpec spec;
      spec.track = *event->descriptor;
      spec.track.id = i ? "receive-video" : "receive-audio";
      spec.output.frame = [&, i](const livekit::sdk::media::FrameView& frame) {
        check(frame.owner && frame.timing.epoch && !frame.planes.empty(),
              "Received frame lacks owning storage or epoch");
        if (!frame.timing.sender_capture_time_us || !frame.timing.presentation_time) {
          return;  // Startup frames can precede the first clock extension.
        }
        check(frame.timing.presentation_clock != nullptr, "Missing shared presentation clock");
        std::lock_guard lock(mutex);
        auto& observation = observed[i];
        ++observation.frames;
        if (!i) {
          check(frame.sample_rate == 48000 && frame.channels == 2, "Ordinary audio format changed");
          observation.ten_ms_audio = frame.planes[0].size() == 480 * 2 * sizeof(int16_t);
        } else {
          check(frame.width == 64 && frame.height == 48, "Ordinary video dimensions changed");
        }
        observation.retained = frame;
      };
      auto operation = await(rx->subscribe_media(spec));
      subscriptions[i] = operation.track;
    }
    while (true) {
      for (const auto& track : publications) {
        require_result(track->check());
      }
      for (const auto& track : subscriptions) {
        require_result(track->check());
      }
      {
        std::lock_guard lock(mutex);
        if (observed[0].frames >= 30 && observed[1].frames >= 10) {
          const auto& audio = observed[0].retained->timing;
          const auto& video = observed[1].retained->timing;
          check(audio.presentation_clock == video.presentation_clock,
                "Participant A/V tracks do not share a presentation clock");
          const auto local_delta = std::chrono::duration_cast<std::chrono::microseconds>(
                                       *audio.presentation_time - *video.presentation_time)
                                       .count();
          const auto sender_delta = *audio.sender_capture_time_us - *video.sender_capture_time_us;
          check(std::abs(local_delta - sender_delta) <= 1,
                "A/V presentation does not preserve sender capture offset");
          check(observed[0].ten_ms_audio, "Ordinary audio bypassed NetEq 10 ms pacing");
          break;
        }
      }
      check(SteadyClock::now() < deadline, "Missing timed audio/video through SFU");
      std::this_thread::sleep_for(10ms);
    }
    check(tx->room().snapshot().state == ConnectionState::connected &&
              rx->room().snapshot().state == ConnectionState::connected,
          "Room not connected");
    await(tx->leave());
    await(rx->leave());
    require_result(tx->close());
    require_result(rx->close());
    for (const auto& observation : observed) {
      check(observation.retained->owner && !observation.retained->planes[0].empty(),
            "Frame storage did not survive shutdown");
      const auto first = observation.retained->planes[0][0];
      (void)first;
    }
    std::cout << "{\"ordinaryMedia\":true,\"synchronousControl\":true,\"netEq10ms\":true,"
                 "\"senderClock\":true,\"avPresentation\":true,\"retainedFrames\":true}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
