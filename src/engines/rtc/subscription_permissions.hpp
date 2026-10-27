#pragma once
#include "engines/rtc/data_messages.hpp"
#include "livekit/participant.hpp"
#include <map>
#include <set>
namespace livekit::sdk::detail {
inline void validate_permissions(const SubscriptionPermissions& policy) {
  if(policy.participants.size()>64 || (policy.all_participants && !policy.participants.empty())) throw std::invalid_argument("Invalid subscription permission list");
  size_t bytes=0;std::set<std::string> identities;
  for(const auto& p:policy.participants) {
    if(p.participant_identity.empty() || p.participant_identity.size()>1024 || !valid_utf8(p.participant_identity) ||
        !identities.insert(p.participant_identity).second || p.track_client_ids.size()>128 || (p.all_tracks && !p.track_client_ids.empty()))
      throw std::invalid_argument("Invalid participant track permission");
    bytes+=p.participant_identity.size();std::set<std::string> tracks;
    for(const auto& id:p.track_client_ids) {
      if(id.empty() || id.size()>256 || !valid_utf8(id) || !tracks.insert(id).second) throw std::invalid_argument("Invalid permission source ID");
      bytes+=id.size();
    }
    if(bytes>32000)throw std::invalid_argument("Subscription permission size limit");
  }
}
inline Json permissions_json(const SubscriptionPermissions& policy) {
  validate_permissions(policy);Json participants=Json::array();
  for(const auto& p:policy.participants)participants.push_back({{"identity",p.participant_identity},{"all",p.all_tracks},{"sources",p.track_client_ids}});
  return {{"all",policy.all_participants},{"participants",participants}};
}
inline SubscriptionPermissions parse_permissions(const Json& value) {
  SubscriptionPermissions policy;policy.all_participants=value.at("all");
  for(const auto& p:value.at("participants"))policy.participants.push_back({p.at("identity"),p.at("all"),p.at("sources").get<std::vector<std::string>>()});
  validate_permissions(policy);return policy;
}
inline livekit::SubscriptionPermission permissions_wire(const SubscriptionPermissions& policy,const std::map<std::string,std::string>& sid_to_source) {
  validate_permissions(policy);livekit::SubscriptionPermission result;result.set_all_participants(policy.all_participants);
  for(const auto& p:policy.participants) {
    auto* permission=result.add_track_permissions();permission->set_participant_identity(p.participant_identity);permission->set_all_tracks(p.all_tracks);
    // Missing/unpublished sources never expand access; they remain denied.
    for(const auto& source:p.track_client_ids)for(const auto& [sid,id]:sid_to_source)if(id==source)permission->add_track_sids(sid);
  }
  return result;
}
}
