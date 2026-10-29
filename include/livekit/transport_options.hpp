#pragma once
namespace livekit::sdk {
// Direct media transport selection. Explicit UDP/TCP modes disable TURN.
enum class MediaTransport { automatic, udp_only, tcp_only };
}
