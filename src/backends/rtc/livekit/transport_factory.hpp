#pragma once
#include "backends/rtc/room_transport.hpp"
#include <functional>

namespace livekit::sdk::rtc::detail {
std::function<std::unique_ptr<RoomTransport>()> livekit_transport_factory();
}  // namespace livekit::sdk::rtc::detail
