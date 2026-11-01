#include "livekit/rtc/room_engine.hpp"
#include "backends/rtc/livekit/transport_factory.hpp"

namespace livekit::sdk::rtc {
std::unique_ptr<RoomEngine> create_livekit_room() {
  return std::make_unique<RoomEngine>(detail::livekit_transport_factory());
}
}  // namespace livekit::sdk::rtc
