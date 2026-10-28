#pragma once
#include <livekit/room.hpp>
#include <cstdlib>

inline void test_media_transport(livekit::sdk::RoomOptions& options) {
  const auto value = std::getenv("LIVEKIT_TEST_MEDIA_TRANSPORT");
  if (!value || std::string(value) == "auto") return;
  if (std::string(value) == "udp") options.media_transport = livekit::sdk::MediaTransport::udp_only;
  else if (std::string(value) == "tcp") options.media_transport = livekit::sdk::MediaTransport::tcp_only;
  else throw std::invalid_argument("Invalid LIVEKIT_TEST_MEDIA_TRANSPORT");
}
