#pragma once
#include "livekit/compat/thread.hpp"
#include <chrono>

namespace livekit::sdk {
struct OperationOptions {
  // Zero uses the service default (30 seconds).
  // Explicit timeouts must also be at most five minutes.
  std::chrono::milliseconds timeout{0};
  livekit::compat::stop_token stop;
  // Reserved for acknowledgement-capable operations. RPC/stream/data-track
  // operations reject this flag; their own protocol defines confirmation.
  bool wait_for_ack = false;
};
}  // namespace livekit::sdk
