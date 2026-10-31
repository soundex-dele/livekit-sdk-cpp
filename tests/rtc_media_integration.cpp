#include "rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/livekit.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <condition_variable>

using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::engine;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct Observation {
  uint64_t frames = 0, decoded = 0;
  bool ten_ms_audio = false;
  std::optional<livekit::sdk::media::FrameView> retained;
};
}  // namespace

// Isolated-SFU integration using only the ordinary public API. Generated media
// and application callbacks avoid opening any physical capture/playout device.
int main() {
  try {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<Observation, 2> observed;
    auto tx = require_result(create_livekit_engine(AudioOutputOptions{}));
    auto rx = require_result(create_livekit_engine(AudioOutputOptions{}));
    std::array<std::shared_ptr<MediaTrack>, 2> publications, subscriptions;
    const bool require_sender_clock = std::getenv("LIVEKIT_TEST_OBSERVER_ONLY") == nullptr;
    int joined = 0, left = 0, published = 0, subscribed = 0, statistics = 0;
    auto on_published = [&](Result<MediaPublication> result) {
      auto publication = require_result(std::move(result));
      std::lock_guard lock(mutex);
      ++published;
      changed.notify_all();
      publications[publication.id == "ordinary-video" ? 1 : 0] = publication.track;
    };
    auto on_subscribed = [&](Result<MediaSubscription> result) {
      auto subscription = require_result(std::move(result));
      std::lock_guard lock(mutex);
      ++subscribed;
      changed.notify_all();
      subscriptions[subscription.id == "receive-video" ? 1 : 0] = subscription.track;
    };
    auto on_left = [&](Result<void> result) {
      require_result(std::move(result));
      std::lock_guard lock(mutex);
      ++left;
    };
    auto tx_joined = [&](Result<RoomSnapshot> result) {
      const auto room = require_result(std::move(result));
      check(room.session != nullptr, "Join callback missing roster");
      {
        std::lock_guard lock(mutex);
        ++joined;
        changed.notify_all();
      }
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
        tx->publish_media(std::move(spec), on_published);
      }
    };
    auto rx_joined = [&](Result<RoomSnapshot> result) {
      require_result(std::move(result));
      std::lock_guard lock(mutex);
      ++joined;
      changed.notify_all();
    };

    struct Observer : RtcObserver {
      std::function<void(const TrackDescriptor&)> discovered;
      std::function<void()> statistics_received;

      void on_room_event(const RoomEvent& event) override {
        if (event.kind == RoomEventKind::remote_track_added && event.descriptor) {
          discovered(*event.descriptor);
        }
        if (event.kind == RoomEventKind::statistics && event.statistics) {
          statistics_received();
        }
      }
    } receiving;

    receiving.statistics_received = [&] {
      std::lock_guard lock(mutex);
      ++statistics;
      changed.notify_all();
    };
    receiving.discovered = [&](const TrackDescriptor& track) {
      const size_t i = track.kind == MediaKind::video ? 1 : 0;
      MediaSubscribeSpec spec;
      spec.track = track;
      spec.track.id = i ? "receive-video" : "receive-audio";
      spec.output.frame = [&, i](const livekit::sdk::media::FrameView& frame) {
        std::lock_guard lock(mutex);
        ++observed[i].decoded;
        check(frame.owner && frame.timing.epoch && !frame.planes.empty(),
              "Received frame lacks owning storage or epoch");
        if (require_sender_clock &&
            (!frame.timing.sender_capture_time_us || !frame.timing.presentation_time)) {
          return;
        }
        if (require_sender_clock) {
          check(frame.timing.presentation_clock != nullptr, "Missing shared presentation clock");
        }
        auto& observation = observed[i];
        ++observation.frames;
        if (!i) {
          check(frame.sample_rate == 48000 && frame.channels == 2, "Ordinary audio format changed");
          observation.ten_ms_audio = frame.planes[0].size() == 480 * 2 * sizeof(int16_t);
        } else {
          check(frame.width == 64 && frame.height == 48, "Ordinary video dimensions changed");
        }
        observation.retained = frame;
        changed.notify_all();
      };
      rx->subscribe_media(std::move(spec), on_subscribed);
    };
    require_result(rx->set_observer(&receiving));
    LiveKitRoomOptions options;
    options.url = std::getenv("LIVEKIT_URL");
    options.token = std::getenv("LIVEKIT_PUBLISH_TOKEN");
    options.stats_interval = 200ms;
    tx->join(options, tx_joined);
    options.token = std::getenv("LIVEKIT_SUBSCRIBE_TOKEN");
    rx->join(options, rx_joined);
    {
      std::unique_lock lock(mutex);
      const bool received = changed.wait_for(lock, 20s, [&] {
        return joined == 2 && published == 2 && subscribed == 2 && observed[0].frames >= 30 &&
               observed[1].frames >= 10 && statistics >= 2;
      });
      check(received, "Missing pushed callbacks or decoded audio/video through SFU");
      if (require_sender_clock) {
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
      }
      check(observed[0].ten_ms_audio, "Ordinary audio bypassed NetEq 10 ms pacing");
      for (const auto& track : publications) {
        require_result(track->check());
      }
      for (const auto& track : subscriptions) {
        require_result(track->check());
      }
    }
    check(tx->snapshot().state == ConnectionState::connected &&
              rx->snapshot().state == ConnectionState::connected,
          "Room not connected");
    check(joined == 2 && published == 2 && subscribed == 2 && subscriptions[0] && subscriptions[1],
          "Missing observer callbacks");
    tx->leave(on_left);
    rx->leave(on_left);
    require_result(rx->set_observer(nullptr));
    check(left == 2, "Missing leave callbacks");
    for (const auto& observation : observed) {
      check(observation.retained->owner && !observation.retained->planes[0].empty(),
            "Frame storage did not survive shutdown");
      const auto first = observation.retained->planes[0][0];
      (void)first;
    }
    std::cout << "{\"ordinaryMedia\":true,\"observerCallbacks\":true,\"netEq10ms\":true,"
                 "\"joinCallbacks\":"
              << joined << ",\"publicationCallbacks\":" << published
              << ",\"subscriptionCallbacks\":" << subscribed << ",\"leaveCallbacks\":" << left
              << ",\"statisticsCallbacks\":" << statistics
              << ",\"audioFrames\":" << observed[0].frames
              << ",\"videoFrames\":" << observed[1].frames
              << ",\"senderClock\":" << (require_sender_clock ? "true" : "false")
              << ",\"avPresentation\":" << (require_sender_clock ? "true" : "false")
              << ",\"retainedFrames\":true}\n";

  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
