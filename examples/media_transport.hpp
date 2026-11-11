#pragma once
#include <livekit/client/types.hpp>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace livekit::sdk::examples {
// Explicit modes select direct ICE transport and disable TURN in the SDK.
inline bool configure_media_transport(client::ConnectionOptions& options) {
  const char* value = std::getenv("LIVEKIT_MEDIA_TRANSPORT");
  const std::string_view mode = value ? value : "automatic";
  if (mode == "automatic") {
    options.media_transport = MediaTransport::automatic;
  } else if (mode == "udp") {
    options.media_transport = MediaTransport::udp_only;
  } else if (mode == "tcp") {
    options.media_transport = MediaTransport::tcp_only;
  } else {
    std::cerr << "LIVEKIT_MEDIA_TRANSPORT must be automatic, udp or tcp\n";
    return false;
  }
  std::cout << "Requested ICE media transport: " << mode << '\n';
  return true;
}
}  // namespace livekit::sdk::examples
