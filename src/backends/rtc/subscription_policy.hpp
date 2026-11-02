#pragma once
#include "livekit/rtc/types.hpp"
#include "support/utf8.hpp"
#include <set>
#include "livekit/result.hpp"

namespace livekit::sdk::rtc::detail {
inline Result<void> validate_subscription_permissions(const SubscriptionPermissions& policy) {
  if (policy.participants.size() > 64 || (policy.all_participants && !policy.participants.empty()))
    return Result<void>::failure(make_failure(ErrorDomain::protocol, ErrorCode::invalid_argument,
                                              "Invalid subscription permission list", false, 0,
                                              {"subscription_permissions"}));
  size_t bytes = 0;
  std::set<std::string> identities;
  for (const auto& participant : policy.participants) {
    const auto& identity = participant.participant_identity;
    if (identity.empty() || identity.size() > 1024 || !livekit::sdk::detail::valid_utf8(identity) ||
        !identities.insert(identity).second || participant.track_ids.size() > 128 ||
        (participant.all_tracks && !participant.track_ids.empty()))
      return Result<void>::failure(make_failure(ErrorDomain::protocol, ErrorCode::invalid_argument,
                                                "Invalid participant subscription permission",
                                                false, 0, {"subscription_permissions"}));
    bytes += identity.size();
    std::set<TrackId> tracks;
    for (const auto& id : participant.track_ids) {
      if (id.empty() || id.size() > 256 || !livekit::sdk::detail::valid_utf8(id) ||
          !tracks.insert(id).second)
        return Result<void>::failure(make_failure(
            ErrorDomain::protocol, ErrorCode::invalid_argument, "Invalid permission publication ID",
            false, 0, {"subscription_permissions"}));
      bytes += id.size();
    }
    if (bytes > 32000)
      return Result<void>::failure(make_failure(ErrorDomain::protocol, ErrorCode::invalid_argument,
                                                "Subscription permission size limit", false, 0,
                                                {"subscription_permissions"}));
  }
  return Result<void>::success();
}
}  // namespace livekit::sdk::rtc::detail
