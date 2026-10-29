#pragma once
#include "backends/rtc/livekit/reconnect_policy.hpp"
#include "backends/rtc/livekit/region_policy.hpp"

namespace livekit::sdk::detail {
// Connection recovery survives native peer replacement. Media restart policy
// and desired sources are owned by the engine; this state knows neither.
struct SignalRecoveryState {
  RecoveryBudget budget;
  bool ever_connected = false, rejoining = false;
  std::string token;
  RegionState regions;
  RecoveryBudget::Clock::time_point session_deadline = RecoveryBudget::Clock::time_point::max();
  RecoveryBudget::Clock::time_point initial_deadline = RecoveryBudget::Clock::time_point::max();
};
}  // namespace livekit::sdk::detail
