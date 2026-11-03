#include "livekit/client/room.hpp"
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <map>
#include <thread>
using namespace livekit::sdk::client;
using namespace std::chrono_literals;

struct Consumer {
  RemoteTrack track;
  size_t frames = 0, bytes = 0;
};

struct Events final : RoomObserver {
  void on_room_event(const RoomEvent& event) override {
    if (event.failure) {
      std::cerr << event.failure->message << '\n';
    }
  }
};

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr
        << "Usage: livekit_sdk_example URL [publish|subscribe] [SECONDS]; set LIVEKIT_TOKEN\n";
    return 2;
  }
  const std::string mode = argc > 2 ? argv[2] : "subscribe";
  if (mode != "publish" && mode != "subscribe") {
    return 2;
  }
  int seconds = 10;
  if (argc > 3) {
    const std::string_view text(argv[3]);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || seconds <= 0) {
      std::cerr << "SECONDS must be a positive integer\n";
      return 2;
    }
  }
  Events events;
  Room room;
  room.set_observer(&events);
  ConnectionOptions options;
  options.url = argv[1];
  if (const auto token = std::getenv("LIVEKIT_TOKEN")) {
    options.token = token;
  }
  options.auto_subscribe = false;
  if (!room.connect(options)) {
    return 1;
  }
  std::vector<LocalTrack> tracks;
  std::map<TrackSid, Consumer> consumers;
  if (mode == "publish") {
    for (int i = 0; i < 2; ++i) {
      auto track = LocalTrack::create_audio("audio-" + std::to_string(i), {Codec::opus, 48000, 2});
      if (!track) {
        std::cerr << track.error().message << '\n';
        return 1;
      }
      auto publication = room.local_participant().publish_track(*track);
      if (!publication) {
        std::cerr << publication.error().message << '\n';
        return 1;
      }
      tracks.push_back(*track);
    }
  }
  const auto started = SteadyClock::now();
  const auto end = started + std::chrono::seconds(seconds);
  uint64_t index = 0;
  while (SteadyClock::now() < end) {
    if (room.connection_state() == ConnectionState::failed ||
        room.connection_state() == ConnectionState::closed) {
      return 1;
    }
    if (mode == "subscribe") {
      for (const auto& peer : room.remote_participants()) {
        for (auto publication : peer.track_publications()) {
          auto info = publication.snapshot();
          if (consumers.count(info.sid)) {
            continue;
          }
          publication.set_subscribed(true);
          auto track = publication.track();
          if (!track) {
            if (publication.snapshot().state == TrackState::closed) {
              continue;
            }
            std::cerr << "Subscription did not become ready\n";
            return 1;
          }
          consumers.emplace(info.sid, Consumer{*track});
        }
      }
    }
    for (auto& track : tracks) {
      EncodedFrame frame;
      frame.buffer = EncodedBuffer({0xf8, 0xff, 0xfe});  // Pre-encoded 20 ms Opus silence.
      frame.epoch = track.feedback().epoch;
      frame.timestamp = index * 960;
      frame.duration = 20ms;
      auto status = track.try_write(std::move(frame));
      if (status == WriteStatus::invalid_frame || status == WriteStatus::closed) {
        return 1;
      }
    }
    for (auto& [sid, consumer] : consumers) {
      // Bound work per track so a busy publisher cannot starve the others.
      for (int count = 0; count < 32; ++count) {
        auto result = consumer.track.read({0ms, {}});
        if (result.status == ReadStatus::failed) {
          return 1;
        }
        if (!result.frame) {
          break;
        }
        ++consumer.frames;
        consumer.bytes += result.frame->buffer.size();
      }
    }
    std::this_thread::sleep_until(started + 20ms * ++index);
  }
  room.close();
  for (const auto& [sid, sink] : consumers) {
    std::cout << sid << ": received " << sink.frames << " encoded frames, " << sink.bytes
              << " bytes\n";
  }
}
