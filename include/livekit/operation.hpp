#pragma once
#include <chrono>
#include <stop_token>

namespace livekit::sdk {
struct OperationOptions {
  // Zero uses RoomOptions.connect_timeout, capped at five minutes.
  // Explicit timeouts must also be at most five minutes.
  std::chrono::milliseconds timeout{0};
  std::stop_token stop;
  // With RoomOptions.signal_protocol = 15, metadata setters may wait for
  // LiveKit RequestResponse (servers lacking it time out). Other controls
  // currently reject this option; publish_track already waits for publication.
  bool wait_for_ack = false;
};
}
