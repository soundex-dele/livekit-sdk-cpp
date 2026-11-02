#pragma once
#include "livekit/api_client.hpp"

namespace livekit::sdk {
class EgressClient : public ServiceClient {
 public:
  using ServiceClient::ServiceClient;
  Result<Json> start_egress(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> start_room_composite_egress(const Json& request,
                                           const RequestOptions& options = {}) const;
  Result<Json> start_participant_egress(const Json& request,
                                        const RequestOptions& options = {}) const;
  Result<Json> start_track_composite_egress(const Json& request,
                                            const RequestOptions& options = {}) const;
  Result<Json> start_track_egress(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> start_web_egress(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> update_layout(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> update_stream(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> list_egress(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> stop_egress(const Json& request, const RequestOptions& options = {}) const;
};

class IngressClient : public ServiceClient {
 public:
  using ServiceClient::ServiceClient;
  Result<Json> create_ingress(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> update_ingress(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> list_ingress(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> delete_ingress(const Json& request, const RequestOptions& options = {}) const;
};

class SIPClient : public ServiceClient {
 public:
  using ServiceClient::ServiceClient;
  Result<Json> create_sip_inbound_trunk(const Json& request,
                                        const RequestOptions& options = {}) const;
  Result<Json> create_sip_outbound_trunk(const Json& request,
                                         const RequestOptions& options = {}) const;
  Result<Json> update_sip_inbound_trunk(const Json& request,
                                        const RequestOptions& options = {}) const;
  Result<Json> update_sip_outbound_trunk(const Json& request,
                                         const RequestOptions& options = {}) const;
  Result<Json> list_sip_trunk(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> list_sip_inbound_trunk(const Json& request,
                                      const RequestOptions& options = {}) const;
  Result<Json> list_sip_outbound_trunk(const Json& request,
                                       const RequestOptions& options = {}) const;
  Result<Json> delete_sip_trunk(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> create_sip_dispatch_rule(const Json& request,
                                        const RequestOptions& options = {}) const;
  Result<Json> update_sip_dispatch_rule(const Json& request,
                                        const RequestOptions& options = {}) const;
  Result<Json> list_sip_dispatch_rule(const Json& request,
                                      const RequestOptions& options = {}) const;
  Result<Json> delete_sip_dispatch_rule(const Json& request,
                                        const RequestOptions& options = {}) const;
  Result<Json> create_sip_participant(const Json& request,
                                      const RequestOptions& options = {}) const;
  Result<Json> transfer_sip_participant(const Json& request,
                                        const RequestOptions& options = {}) const;
};

class AgentDispatchClient : public ServiceClient {
 public:
  // Returns JSON null when no matching dispatch exists (Go GetDispatch semantics).
  Result<Json> get_dispatch(const std::string& id, const std::string& room,
                            const RequestOptions& options = {}) const;
  using ServiceClient::ServiceClient;
  Result<Json> create_dispatch(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> delete_dispatch(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> list_dispatch(const Json& request, const RequestOptions& options = {}) const;
};

class ConnectorClient : public ServiceClient {
 public:
  using ServiceClient::ServiceClient;
  Result<Json> connect_twilio_call(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> dial_whats_app_call(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> accept_whats_app_call(const Json& request, const RequestOptions& options = {}) const;
  Result<Json> connect_whats_app_call(const Json& request,
                                      const RequestOptions& options = {}) const;
  Result<Json> disconnect_whats_app_call(const Json& request,
                                         const RequestOptions& options = {}) const;
};
}  // namespace livekit::sdk
