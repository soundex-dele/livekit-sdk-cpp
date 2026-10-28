#pragma once
#include "engines/rtc/capabilities.hpp"
#include "livekit/room.hpp"
#include <source_location>
inline void capabilities_contract(){
  using namespace livekit::sdk;
  auto check=[](bool ok,std::source_location at=std::source_location::current()){if(!ok)throw std::runtime_error("Capabilities contract failed at "+std::to_string(at.line()));};
  const auto sdk=sdk_capabilities();
  check(sdk.version=="0.3.0"&&sdk.signal_protocols==std::vector<int>({12,15,17}));
  check(sdk.support(ClientFeature::rpc)==CapabilitySupport::supported);
  check(sdk.support(ClientFeature::data_e2ee)==CapabilitySupport::supported);
  check(sdk.support(static_cast<ClientFeature>(999))==CapabilitySupport::unknown);
  Room room;auto snapshot=room.capabilities();check(!snapshot.connected&&!snapshot.signal_protocol&&!snapshot.server.advertised_protocol);
  livekit::JoinResponse join;join.set_server_version("legacy");join.set_subscriber_primary(true);
  detail::update_capabilities(snapshot,detail::session_capabilities_json(join,15,true));
  check(snapshot.signal_protocol==15&&snapshot.relay_only==true&&snapshot.subscriber_primary==true&&!snapshot.single_peer_connection);
  detail::update_capabilities(snapshot,{{"singlePeerConnection",true}});check(snapshot.single_peer_connection==true);
  check(snapshot.server.version=="legacy"&&!snapshot.server.enabled_publish_codecs&&!snapshot.server.advertised_protocol);
  check(snapshot.server.support(ClientFeature::rpc)==CapabilitySupport::unknown);
  join.mutable_server_info()->set_version("new");join.mutable_server_info()->set_protocol(18);join.mutable_server_info()->set_region("test");
  auto* codec=join.add_enabled_publish_codecs();codec->set_mime("video/VP8");codec->set_fmtp_line("x=1");
  detail::update_capabilities(snapshot,detail::session_capabilities_json(join,12,false));
  check(snapshot.server.advertised_protocol==18&&snapshot.signal_protocol==12);
  check(snapshot.server.version=="new"&&snapshot.server.region=="test"&&snapshot.server.enabled_publish_codecs->at(0).fmtp_line=="x=1");
  detail::update_capabilities(snapshot,{{"relayOnly",true}});check(snapshot.server.version=="new");
  detail::update_capabilities(snapshot,{{"server",Json::object()}});
  check(snapshot.server.version.empty()&&!snapshot.server.advertised_protocol&&!snapshot.server.enabled_publish_codecs);
}
