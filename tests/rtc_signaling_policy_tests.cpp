#include "region_policy_contract.hpp"
#include "token_provider_contract.hpp"
#include "backends/rtc/livekit/reconnect_policy.hpp"
#include <cstdlib>

int main() {
  region_policy_contract();
  token_provider_contract();
  using namespace livekit::sdk;
  ReconnectOptions options;
  options.max_attempts = 2;
  detail::RecoveryBudget budget(options);
  auto first = budget.next_attempt();
  auto second = budget.next_attempt();
  auto exhausted = budget.next_attempt();
  if (!first || *first != 1 || !second || *second != 2 || exhausted ||
      exhausted.error().code != ErrorCode::recovery_failed) {
    return 1;
  }
  options.max_attempts = 0;
  auto invalid = detail::validate_reconnect(options);
  if (invalid || invalid.error().code != ErrorCode::invalid_argument) {
    return 1;
  }
  return 0;
}
