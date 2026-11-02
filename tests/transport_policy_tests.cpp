#include "livekit/result.hpp"
#include <cstdlib>
#include "backends/webrtc/transport_policy.hpp"
#include "backends/rtc/livekit/client_configuration.hpp"
#include <iostream>

using namespace livekit::sdk;

static void check(bool value) {
  if (!value) {
    std::cerr << "transport policy assertion failed\n";
    std::exit(1);
  }
}

int main() {
    using PC = webrtc::PeerConnectionInterface;
    PC::RTCConfiguration defaults;
    auto automatic = defaults;
    check(bool(detail::apply_media_transport(automatic, MediaTransport::automatic, false)));
    check(automatic == defaults);
    for (auto policy : {MediaTransport::udp_only, MediaTransport::tcp_only}) {
      PC::RTCConfiguration config;
      config.port_allocator_config.flags |= webrtc::PORTALLOCATOR_ENABLE_IPV6;
      check(bool(detail::apply_media_transport(config, policy, false)));
      check(config.port_allocator_config.flags & webrtc::PORTALLOCATOR_DISABLE_RELAY);
      check(config.port_allocator_config.flags & webrtc::PORTALLOCATOR_ENABLE_IPV6);
      check(bool(config.port_allocator_config.flags & webrtc::PORTALLOCATOR_DISABLE_UDP) ==
            (policy == MediaTransport::tcp_only));
      check(bool(config.port_allocator_config.flags & webrtc::PORTALLOCATOR_DISABLE_TCP) ==
            (policy == MediaTransport::udp_only));
      check((config.tcp_candidate_policy == PC::kTcpCandidatePolicyDisabled) ==
            (policy == MediaTransport::udp_only));
      auto resumed = config;
      check(bool(detail::apply_media_transport(resumed, policy, false)));
      check(resumed == config);
      livekit::ClientConfiguration forced;
      forced.set_force_relay(livekit::ENABLED);
      auto relay = detail::relay_required(false, policy != MediaTransport::automatic, forced);
      check(!relay && relay.error().code == ErrorCode::unsupported);
    }
    check(bool(detail::apply_media_transport(automatic, MediaTransport::automatic, true)));
    check(automatic.port_allocator_config.flags & webrtc::PORTALLOCATOR_DISABLE_RELAY);
    auto invalid = defaults;
    auto rejected = detail::apply_media_transport(invalid, static_cast<MediaTransport>(99), true);
    check(!rejected && rejected.error().code == ErrorCode::invalid_argument);
    check(invalid == defaults);
    std::cout << "strict transport policy passed\n";
}
