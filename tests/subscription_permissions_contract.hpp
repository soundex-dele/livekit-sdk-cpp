#pragma once
#include "engines/rtc/subscription_permissions.hpp"
inline void subscription_permissions_contract() {
  using namespace livekit::sdk;
  auto check=[](bool ok){if(!ok)throw std::runtime_error("Subscription permission contract failed");};
  SubscriptionPermissions permissions{false,{{"viewer",false,{"video-1"}},{"monitor",true,{}}}};
  auto copy=detail::parse_permissions(detail::permissions_json(permissions));
  auto denied=detail::permissions_wire(copy,{});check(!denied.all_participants() && denied.track_permissions(0).track_sids().empty());
  auto old=detail::permissions_wire(copy,{{"TR_old","video-1"}});
  auto fresh=detail::permissions_wire(copy,{{"TR_new","video-1"}});
  check(old.track_permissions(0).track_sids(0)=="TR_old" && fresh.track_permissions(0).track_sids(0)=="TR_new");
  check(fresh.track_permissions(1).all_tracks() && fresh.track_permissions(1).participant_identity()=="monitor");
  auto reject=[&](SubscriptionPermissions p){bool threw=false;try{detail::validate_permissions(p);}catch(const std::invalid_argument&){threw=true;}check(threw);};
  reject({true,{{"viewer",true,{}}}});reject({false,{{"viewer",true,{"id"}}}});
  reject({false,{{"viewer",false,{"id","id"}}}});reject({false,{{"",true,{}}}});
  reject({false,{{"viewer",true,{}},{"viewer",true,{}}}});
}
