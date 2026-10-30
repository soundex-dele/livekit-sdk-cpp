#include "rtc_blocking_bridge.hpp"
#include "rtc_event_mailbox.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace livekit::sdk::rtc::engine;
using namespace livekit::sdk::app_support;
using namespace std::chrono_literals;

namespace {
void check(bool value, const char* message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

struct Client {
  std::unique_ptr<BlockingRtc> engine =
      require_result(create_blocking_engine(AudioOutputOptions{}));
  RtcEventMailbox events{*engine};
  std::map<std::string, std::shared_ptr<std::atomic<int>>> received;
  std::map<std::string, Codec> codecs;
  bool active = false;

  void join(const char* token, bool h265) {
    LiveKitRoomOptions options;
    options.url = std::getenv("LIVEKIT_URL");
    options.token = std::getenv(token);
    options.signal_protocol = 15;
    options.video_codec_policy = RoomVideoCodecPolicy{h265};
    require_result(engine->join(options));
    active = true;
  }

  void pump() {
    if (!active) {
      return;
    }
    auto policy = engine->raw().video_codec_policy();
    if (policy.failure) {
      throw std::runtime_error(policy.failure->message);
    }
    while (auto event = events.take_event()) {
      if (event->kind != RoomEventKind::remote_track_added || !event->descriptor ||
          event->descriptor->kind != MediaKind::video) {
        continue;
      }
      const auto& descriptor = *event->descriptor;
      const auto& sid = descriptor.remote_sid;
      if (received.contains(sid) ||
          (policy.h264_latched && descriptor.format.codec != Codec::h264)) {
        continue;
      }
      // An old add notification may have been queued before a replacement.
      auto session = engine->room().snapshot().session;
      bool present = false;
      if (session) {
        for (const auto& [id, participant] : session->remote) {
          present = present || participant.publications.contains(sid);
        }
      }
      if (!present) {
        continue;
      }
      auto count = std::make_shared<std::atomic<int>>(0);
      MediaSubscribeSpec spec;
      spec.track = descriptor;
      spec.track.id = "receive." + sid;
      spec.synchronize = false;
      spec.output.video = [count](const VideoSample& sample) {
        check(sample.pixels && sample.pixels->width == 64 && sample.pixels->height == 48,
              "Wrong decoded replacement pixels");
        ++*count;
      };
      auto result = engine->subscribe_media(spec);
      if (!result) {
        throw std::runtime_error(result.error().message);
      }
      received[sid] = count;
      codecs[sid] = descriptor.format.codec;
    }
  }

  bool decoded(size_t expected, Codec codec) {
    auto session = engine->room().snapshot().session;
    if (!session) {
      return false;
    }
    size_t tracks = 0;
    for (const auto& [id, participant] : session->remote) {
      for (const auto& [sid, track] : participant.publications) {
        if (track.kind != PublicationKind::video) {
          continue;
        }
        ++tracks;
        if (!received.contains(sid) || *received.at(sid) < 3 || codecs.at(sid) != codec) {
          return false;
        }
      }
    }
    return tracks == expected;
  }

  void publish(const std::string& name) {
    MediaPublishSpec spec;
    spec.track.id = spec.track.name = name;
    spec.track.format.codec = Codec::h265;
    spec.track.format.width = 64;
    spec.track.format.height = 48;
    spec.input.generated = true;
    require_result(engine->publish_video(spec));
  }
};

template <class F>
void until(std::array<Client, 4>& clients, F done) {
  const auto deadline = SteadyClock::now() + 30s;
  while (true) {
    for (auto& client : clients) {
      client.pump();
    }
    if (done()) {
      return;
    }
    check(SteadyClock::now() < deadline, "SFU codec-policy decoding timed out");
    std::this_thread::sleep_for(10ms);
  }
}
}  // namespace

int main() {
  try {
    std::array<Client, 4> clients;
    clients[0].join("LIVEKIT_CODEC_PUBLISHER_A", true);
    clients[1].join("LIVEKIT_CODEC_PUBLISHER_B", true);
    for (size_t i = 0; i < 2; ++i) {
      clients[i].publish("front");
      clients[i].publish("rear");
    }
    until(clients,
          [&] { return clients[0].decoded(2, Codec::h265) && clients[1].decoded(2, Codec::h265); });
    std::cerr << "Initial four H265 tracks decoded\n";
    clients[2].join("LIVEKIT_CODEC_H264_VIEWER", false);
    until(clients, [&] {
      return clients[0].decoded(2, Codec::h264) && clients[1].decoded(2, Codec::h264) &&
             clients[2].decoded(4, Codec::h264);
    });
    std::cerr << "All eight H264 receive links decoded after fallback\n";
    require_result(clients[2].engine->leave());
    clients[2].active = false;
    clients[0].publish("late-track");
    clients[3].join("LIVEKIT_CODEC_LATE_PUBLISHER", true);
    clients[3].publish("late-publisher");
    until(clients, [&] {
      return clients[0].decoded(3, Codec::h264) && clients[1].decoded(4, Codec::h264) &&
             clients[3].decoded(5, Codec::h264);
    });
    for (auto& client : clients) {
      require_result(client.engine->close());
    }
    std::cout << "{\"initialH265Tracks\":4,\"fallbackH264ReceiveLinks\":8,"
                 "\"lateH264ReceiveLinks\":12,\"stickyAfterLeave\":true,"
                 "\"latePublisher\":true,\"decodedPixels\":true}\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
