#pragma once
#include "livekit/reconnect.hpp"
#include "livekit/token_provider.hpp"
#include "livekit/transport_options.hpp"
#include <chrono>
#include <string>
#include <vector>

namespace livekit::sdk::rtc {
// Session/network configuration only. This header deliberately has no media
// source, device, codec implementation, rendering or graph dependency.
struct SignalingOptions {
  std::string url, token;
  bool subscribe = false;
  std::vector<std::string> subscribe_tracks;
  int connect_timeout = 30;
  int signal_protocol = 12;
  bool single_peer_connection = false;
  MediaTransport media_transport = MediaTransport::automatic;
  ReconnectOptions reconnect;
  TokenProvider token_provider;
  std::chrono::milliseconds token_timeout{10000};
  RegionOptions regions;
};
}  // namespace livekit::sdk::rtc
