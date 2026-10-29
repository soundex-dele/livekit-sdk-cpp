#include "backends/webrtc/transport_policy.hpp"
#include "backends/rtc/client_configuration.hpp"
#include <iostream>

using namespace livekit::sdk;

static void check(bool value) {
  if (!value)
    throw std::runtime_error("transport policy assertion failed");
}

int main() {
  try {
    using PC = webrtc::PeerConnectionInterface;
    PC::RTCConfiguration defaults;
    auto automatic = defaults;
    detail::apply_media_transport(automatic, MediaTransport::automatic, false);
    check(automatic == defaults);
    for (auto policy : {MediaTransport::udp_only, MediaTransport::tcp_only}) {
      PC::RTCConfiguration config;
      config.port_allocator_config.flags |= webrtc::PORTALLOCATOR_ENABLE_IPV6;
      detail::apply_media_transport(config, policy, false);
      check(config.port_allocator_config.flags & webrtc::PORTALLOCATOR_DISABLE_RELAY);
      check(config.port_allocator_config.flags & webrtc::PORTALLOCATOR_ENABLE_IPV6);
      check(bool(config.port_allocator_config.flags & webrtc::PORTALLOCATOR_DISABLE_UDP) ==
            (policy == MediaTransport::tcp_only));
      check(bool(config.port_allocator_config.flags & webrtc::PORTALLOCATOR_DISABLE_TCP) ==
            (policy == MediaTransport::udp_only));
      check((config.tcp_candidate_policy == PC::kTcpCandidatePolicyDisabled) ==
            (policy == MediaTransport::udp_only));
      auto resumed = config;
      detail::apply_media_transport(resumed, policy, false);
      check(resumed == config);
      livekit::ClientConfiguration forced;
      forced.set_force_relay(livekit::ENABLED);
      bool rejected = false;
      try {
        detail::relay_required(false, policy != MediaTransport::automatic, forced);
      } catch (const SDKError& e) {
        rejected = e.code() == ErrorCode::unsupported;
      }
      check(rejected);
    }
    detail::apply_media_transport(automatic, MediaTransport::automatic, true);
    check(automatic.port_allocator_config.flags & webrtc::PORTALLOCATOR_DISABLE_RELAY);
    std::cout << "strict transport policy passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
