#pragma once
#include <livekit/room.hpp>

namespace demo {
// Convenience authentication for a trusted local test server.
livekit::sdk::RoomOptions username_options(std::string url, std::string room, std::string username);
}
