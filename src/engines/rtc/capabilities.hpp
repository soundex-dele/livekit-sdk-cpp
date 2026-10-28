#pragma once
#include "livekit/capabilities.hpp"
#include "livekit/auth.hpp"
#include "livekit.pb.h"

namespace livekit::sdk::detail {
inline Json server_capabilities_json(const livekit::ServerInfo& server) {
  Json result{{"version",server.version()},{"region",server.region()}};
  if(server.protocol()>0)result["protocol"]=server.protocol();
  return result;
}
inline Json session_capabilities_json(const livekit::JoinResponse& join,int protocol,bool relay) {
  auto server=join.has_server_info()?server_capabilities_json(join.server_info()):Json{{"version",join.server_version()}};
  if(join.enabled_publish_codecs_size()) {
    server["publishCodecs"]=Json::array();
    for(const auto& codec:join.enabled_publish_codecs())server["publishCodecs"].push_back({{"mime",codec.mime()},{"fmtp",codec.fmtp_line()}});
  }
  return {{"protocol",protocol},{"subscriberPrimary",join.subscriber_primary()},{"relayOnly",relay},{"server",server}};
}
inline void update_capabilities(SessionCapabilities& capabilities,const Json& value) {
  if(value.contains("protocol"))capabilities.signal_protocol=value.at("protocol").get<int>();
  if(value.contains("subscriberPrimary"))capabilities.subscriber_primary=value.at("subscriberPrimary").get<bool>();
  if(value.contains("relayOnly"))capabilities.relay_only=value.at("relayOnly").get<bool>();
  if(value.contains("singlePeerConnection"))capabilities.single_peer_connection=value.at("singlePeerConnection").get<bool>();
  if(value.contains("server")) {
    const auto& server=value.at("server");capabilities.server={};
    capabilities.server.version=server.value("version","");capabilities.server.region=server.value("region","");
    if(server.contains("protocol"))capabilities.server.advertised_protocol=server.at("protocol").get<int>();
    if(server.contains("publishCodecs")) {
      capabilities.server.enabled_publish_codecs.emplace();
      for(const auto& codec:server.at("publishCodecs"))capabilities.server.enabled_publish_codecs->push_back({codec.at("mime"),codec.at("fmtp")});
    }
  }
}
}
