#pragma once
#include "livekit/rtc/room_backend.hpp"

namespace livekit::sdk::rtc {
// Creates the LiveKit signaling/WebRTC adapter. Links LiveKit::RTCLiveKit;
// no engine, media graph, device or actual codec factory is required.
std::unique_ptr<RoomBackend> create_livekit_room();
}  // namespace livekit::sdk::rtc
