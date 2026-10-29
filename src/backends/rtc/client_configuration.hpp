#pragma once
#include "livekit.pb.h"
#include "livekit/error.hpp"

namespace livekit::sdk::detail {
inline bool relay_required(bool user_relay, bool disable_turn, const livekit::ClientConfiguration& config) {
  const bool relay = user_relay || config.force_relay() == livekit::ENABLED;
  if (relay && disable_turn)
    throw SDKError(ErrorDomain::transport, ErrorCode::unsupported, "Relay policy conflicts with disabled TURN");
  return relay;
}
}
