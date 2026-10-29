#pragma once
#include "livekit/rtc/room_session.hpp"

namespace livekit::sdk::rtc {
// Creates the LiveKit signaling/WebRTC adapter. Links LiveKit::RTCLiveKit;
// no engine, media graph, device or actual codec factory is required.
std::unique_ptr<RoomSession> create_livekit_room();
}  // namespace livekit::sdk::rtc
