#pragma once
#include "livekit/result.hpp"
#include <string>
#include <string_view>

namespace livekit::sdk {
// Wire-format helpers shared by RTC signaling and trusted server clients.
// They do not sign tokens, hold API secrets or depend on the ServerSDK.
std::string base64url(std::string_view value);
Result<std::string> base64_decode(std::string_view value);
std::string percent_encode(std::string_view value);
Result<std::string> endpoint(std::string url, bool websocket);
}  // namespace livekit::sdk
