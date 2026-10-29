#pragma once
#include "livekit/rtc/types.hpp"
#include "support/utf8.hpp"
#include <set>
#include <stdexcept>

namespace livekit::sdk::rtc::detail {
inline void validate_subscription_permissions(const SubscriptionPermissions& policy) {
  if (policy.participants.size() > 64 || (policy.all_participants && !policy.participants.empty()))
    throw std::invalid_argument("Invalid subscription permission list");
  size_t bytes = 0;
  std::set<std::string> identities;
  for (const auto& participant : policy.participants) {
    const auto& identity = participant.participant_identity;
    if (identity.empty() || identity.size() > 1024 || !livekit::sdk::detail::valid_utf8(identity) ||
        !identities.insert(identity).second || participant.track_ids.size() > 128 ||
        (participant.all_tracks && !participant.track_ids.empty()))
      throw std::invalid_argument("Invalid participant subscription permission");
    bytes += identity.size();
    std::set<TrackId> tracks;
    for (const auto& id : participant.track_ids) {
      if (id.empty() || id.size() > 256 || !livekit::sdk::detail::valid_utf8(id) ||
          !tracks.insert(id).second)
        throw std::invalid_argument("Invalid permission publication ID");
      bytes += id.size();
    }
    if (bytes > 32000)
      throw std::invalid_argument("Subscription permission size limit");
  }
}
}  // namespace livekit::sdk::rtc::detail
