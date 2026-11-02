#include "livekit/client/room.hpp"

#include <cstdlib>
#include <iostream>
#include <thread>

using namespace livekit::sdk::client;
using namespace std::chrono_literals;

class Events : public RoomObserver {
 public:
  void on_room_event(const RoomEvent& event) override {
    if (event.kind == RoomEventKind::connection_changed) {
      std::cout << "connection state=" << int(event.connection) << '\n';
    }
    // Dispatch any synchronous control to an application thread, not this callback.
  }
};

int main() {
  const char* url = std::getenv("LIVEKIT_URL");
  const char* token = std::getenv("LIVEKIT_TOKEN");
  if (!url || !token) {
    std::cerr << "Set LIVEKIT_URL and LIVEKIT_TOKEN\n";
    return 1;
  }
  Events events;  // Observer outlives Room.
  Room room;
  room.set_observer(&events);
  ConnectionOptions options;
  options.url = url;
  options.token = token;
  options.auto_subscribe = false;
  if (!room.connect(options)) {
    return 1;
  }
  CodecFormat format;
  format.codec = Codec::opus;
  format.clock_rate = 48000;
  format.channels = 1;
  auto track = LocalTrack::create_audio("encoded-opus", format);
  if (!track) {
    return 1;
  }
  auto publication = room.local_participant().publish_track(*track);
  if (!publication) {
    return 1;
  }
  room.local_participant().publish_data({{'h', 'e', 'l', 'l', 'o'}, true, "example", {}});
  const auto started = SteadyClock::now();
  for (uint64_t index = 0; index < 250; ++index) {
    EncodedFrame frame;
    frame.buffer = EncodedBuffer({0xf8, 0xff, 0xfe});  // Already encoded 20 ms Opus silence.
    frame.timestamp = index * 960;
    frame.duration = 20ms;
    frame.epoch = track->feedback().epoch;
    const auto status = track->try_write(std::move(frame));
    if (status == WriteStatus::closed) {
      break;
    }
    std::this_thread::sleep_until(started + 20ms * (index + 1));
  }
  room.local_participant().unpublish_track(publication->snapshot().sid);
  room.close();
}
