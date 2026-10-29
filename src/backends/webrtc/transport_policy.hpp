#pragma once
#include "livekit/transport_options.hpp"
#include "api/peer_connection_interface.h"
#include "p2p/base/port_allocator.h"

namespace livekit::sdk::detail {
inline void apply_media_transport(webrtc::PeerConnectionInterface::RTCConfiguration& config,
                                  MediaTransport transport, bool disable_turn) {
  using PC = webrtc::PeerConnectionInterface;
  if (disable_turn || transport != MediaTransport::automatic)
    config.port_allocator_config.flags |= webrtc::PORTALLOCATOR_DISABLE_RELAY;
  switch (transport) {
    case MediaTransport::automatic: break;
    case MediaTransport::udp_only:
      config.tcp_candidate_policy = PC::kTcpCandidatePolicyDisabled;
      config.port_allocator_config.flags |= webrtc::PORTALLOCATOR_DISABLE_TCP;
      break;
    case MediaTransport::tcp_only:
      config.tcp_candidate_policy = PC::kTcpCandidatePolicyEnabled;
      config.port_allocator_config.flags |= webrtc::PORTALLOCATOR_DISABLE_UDP | webrtc::PORTALLOCATOR_DISABLE_STUN;
      break;
    default: throw std::invalid_argument("Invalid media_transport");
  }
}
}
