// Consumer of signaling/transport only: no private headers or media targets.
#include "livekit/rtc/room_engine.hpp"
#include "livekit/rtc/livekit.hpp"

int main() {
  auto room = livekit::sdk::rtc::create_livekit_room();
  livekit::sdk::rtc::RoomObserver observer;
  if (room->set_observer(&observer) != livekit::sdk::ErrorCode::ok) {
    return 1;
  }
  if (room->snapshot().state != livekit::sdk::rtc::ConnectionState::disconnected) {
    return 2;
  }
  if (room->set_observer(nullptr) != livekit::sdk::ErrorCode::ok) {
    return 3;
  }
  room->leave();
  return 0;
}
