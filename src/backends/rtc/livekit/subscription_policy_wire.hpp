#pragma once
#include "backends/rtc/subscription_policy.hpp"
#include "livekit.pb.h"

namespace livekit::sdk::rtc::detail {
inline livekit::SubscriptionPermission subscription_permissions_wire(
    const SubscriptionPermissions& policy, const std::map<TrackId, std::string>& publication_sids) {
  validate_subscription_permissions(policy);
  livekit::SubscriptionPermission result;
  result.set_all_participants(policy.all_participants);
  for (const auto& participant : policy.participants) {
    auto* entry = result.add_track_permissions();
    entry->set_participant_identity(participant.participant_identity);
    entry->set_all_tracks(participant.all_tracks);
    for (const auto& id : participant.track_ids) {
      const auto found = publication_sids.find(id);
      // Pending/missing local IDs never expand access to other publications.
      if (found != publication_sids.end() && !found->second.empty())
        entry->add_track_sids(found->second);
    }
  }
  return result;
}
}  // namespace livekit::sdk::rtc::detail
