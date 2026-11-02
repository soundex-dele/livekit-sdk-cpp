#pragma once
#include "livekit.pb.h"
#include "livekit/result.hpp"

namespace livekit::sdk::detail {
inline Result<bool> relay_required(bool user_relay, bool disable_turn,
                                   const livekit::ClientConfiguration& config) {
  const bool relay = user_relay || config.force_relay() == livekit::ENABLED;
  if (relay && disable_turn)
    return Result<bool>::failure(make_failure(ErrorDomain::transport, ErrorCode::unsupported,
                                              "Relay policy conflicts with disabled TURN"));
  return Result<bool>::success(relay);
}
}  // namespace livekit::sdk::detail
