#pragma once
#include "livekit/room_service.hpp"
#include "livekit/services.hpp"
namespace livekit::sdk {
struct APIOptions {
  std::string url;
  Credentials credentials;
  bool use_environment = true;
};
// Mirrors Go NewLiveKitAPI: explicit credentials suppress ALL environment credentials.
class LiveKitAPI {
 public:
  explicit LiveKitAPI(APIOptions options = {});
  RoomServiceClient& room() { return room_; }
  EgressClient& egress() { return egress_; }
  IngressClient& ingress() { return ingress_; }
  SIPClient& sip() { return sip_; }
  AgentDispatchClient& agent_dispatch() { return agent_dispatch_; }
  ConnectorClient& connector() { return connector_; }
 private:
  LiveKitAPI(APIOptions resolved, int);
  RoomServiceClient room_;
  EgressClient egress_;
  IngressClient ingress_;
  SIPClient sip_;
  AgentDispatchClient agent_dispatch_;
  ConnectorClient connector_;
};
}
