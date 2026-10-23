#include "livekit/services.hpp"
#include "livekit/room_service.hpp"
#include "lk/api_transport.hpp"
#include <cmath>
#include <limits>
namespace livekit::sdk {
namespace {
RequestOptions dial_options(Json& body, RequestOptions options) {
  const char* key = body.contains("ringing_timeout") ? "ringing_timeout" : "ringingTimeout";
  if (!body.contains(key) || body.at(key).is_null()) body[key] = "30s";
  auto text = body.at(key).get<std::string>();
  if (text.empty() || text.back() != 's') throw std::invalid_argument("ringingTimeout must be a protobuf duration (e.g. 30s)");
  text.pop_back(); size_t consumed = 0; double ring = std::stod(text, &consumed);
  if (consumed != text.size() || !std::isfinite(ring) || ring < 0 || ring > std::numeric_limits<int>::max() - 3)
    throw std::invalid_argument("Invalid ringingTimeout");
  options.timeout = std::max(options.timeout, static_cast<int>(std::ceil(ring + 2)));
  return options;
}
}

Json RoomServiceClient::create_room(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("RoomService", "CreateRoom", body, {{"video", {{"roomCreate", true}}}}, options);
}
Json RoomServiceClient::list_rooms(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("RoomService", "ListRooms", body, {{"video", {{"roomList", true}}}}, options);
}
Json RoomServiceClient::delete_room(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("RoomService", "DeleteRoom", body, {{"video", {{"roomCreate", true}}}}, options);
}
Json RoomServiceClient::list_participants(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("RoomService", "ListParticipants", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}}}}, options);
}
Json RoomServiceClient::get_participant(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("RoomService", "GetParticipant", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}}}}, options);
}
Json RoomServiceClient::remove_participant(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("RoomService", "RemoveParticipant", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}}}}, options);
}
Json RoomServiceClient::forward_participant(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("RoomService", "ForwardParticipant", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}, {"destinationRoom", detail::field(body, "destinationRoom", "destination_room")}}}}, options);
}
Json RoomServiceClient::move_participant(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("RoomService", "MoveParticipant", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}, {"destinationRoom", detail::field(body, "destinationRoom", "destination_room")}}}}, options);
}
Json RoomServiceClient::mute_published_track(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("RoomService", "MutePublishedTrack", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}}}}, options);
}
Json RoomServiceClient::update_participant(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("RoomService", "UpdateParticipant", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}}}}, options);
}
Json RoomServiceClient::update_subscriptions(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("RoomService", "UpdateSubscriptions", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}}}}, options);
}
Json RoomServiceClient::update_room_metadata(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("RoomService", "UpdateRoomMetadata", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}}}}, options);
}
Json RoomServiceClient::send_data(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  body["nonce"] = detail::random_base64(16);
  return request("RoomService", "SendData", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}}}}, options);
}
Json EgressClient::start_egress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Egress", "StartEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}
Json EgressClient::start_room_composite_egress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Egress", "StartRoomCompositeEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}
Json EgressClient::start_participant_egress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Egress", "StartParticipantEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}
Json EgressClient::start_track_composite_egress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Egress", "StartTrackCompositeEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}
Json EgressClient::start_track_egress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Egress", "StartTrackEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}
Json EgressClient::start_web_egress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Egress", "StartWebEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}
Json EgressClient::update_layout(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Egress", "UpdateLayout", body, {{"video", {{"roomRecord", true}}}}, options);
}
Json EgressClient::update_stream(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Egress", "UpdateStream", body, {{"video", {{"roomRecord", true}}}}, options);
}
Json EgressClient::list_egress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Egress", "ListEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}
Json EgressClient::stop_egress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Egress", "StopEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}
Json IngressClient::create_ingress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Ingress", "CreateIngress", body, {{"video", {{"ingressAdmin", true}}}}, options);
}
Json IngressClient::update_ingress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Ingress", "UpdateIngress", body, {{"video", {{"ingressAdmin", true}}}}, options);
}
Json IngressClient::list_ingress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Ingress", "ListIngress", body, {{"video", {{"ingressAdmin", true}}}}, options);
}
Json IngressClient::delete_ingress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Ingress", "DeleteIngress", body, {{"video", {{"ingressAdmin", true}}}}, options);
}
Json SIPClient::create_sip_inbound_trunk(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("SIP", "CreateSIPInboundTrunk", body, {{"sip", {{"admin", true}}}}, options);
}
Json SIPClient::create_sip_outbound_trunk(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("SIP", "CreateSIPOutboundTrunk", body, {{"sip", {{"admin", true}}}}, options);
}
Json SIPClient::update_sip_inbound_trunk(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("SIP", "UpdateSIPInboundTrunk", body, {{"sip", {{"admin", true}}}}, options);
}
Json SIPClient::update_sip_outbound_trunk(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("SIP", "UpdateSIPOutboundTrunk", body, {{"sip", {{"admin", true}}}}, options);
}
Json SIPClient::list_sip_trunk(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("SIP", "ListSIPTrunk", body, {{"sip", {{"admin", true}}}}, options);
}
Json SIPClient::list_sip_inbound_trunk(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("SIP", "ListSIPInboundTrunk", body, {{"sip", {{"admin", true}}}}, options);
}
Json SIPClient::list_sip_outbound_trunk(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("SIP", "ListSIPOutboundTrunk", body, {{"sip", {{"admin", true}}}}, options);
}
Json SIPClient::delete_sip_trunk(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("SIP", "DeleteSIPTrunk", body, {{"sip", {{"admin", true}}}}, options);
}
Json SIPClient::create_sip_dispatch_rule(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("SIP", "CreateSIPDispatchRule", body, {{"sip", {{"admin", true}}}}, options);
}
Json SIPClient::update_sip_dispatch_rule(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("SIP", "UpdateSIPDispatchRule", body, {{"sip", {{"admin", true}}}}, options);
}
Json SIPClient::list_sip_dispatch_rule(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("SIP", "ListSIPDispatchRule", body, {{"sip", {{"admin", true}}}}, options);
}
Json SIPClient::delete_sip_dispatch_rule(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("SIP", "DeleteSIPDispatchRule", body, {{"sip", {{"admin", true}}}}, options);
}
Json SIPClient::create_sip_participant(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  auto effective = options;
  if (body.value("waitUntilAnswered", body.value("wait_until_answered", false))) effective = dial_options(body, options);
  return request("SIP", "CreateSIPParticipant", body, {{"sip", {{"call", true}}}}, effective);
}
Json SIPClient::transfer_sip_participant(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  auto effective = dial_options(body, options);
  return request("SIP", "TransferSIPParticipant", body, {{"sip", {{"call", true}}}, {"video", {{"roomAdmin", true}, {"room", detail::field(body, "roomName", "room_name")}}}}, effective);
}
Json AgentDispatchClient::create_dispatch(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("AgentDispatchService", "CreateDispatch", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}}}}, options);
}
Json AgentDispatchClient::delete_dispatch(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("AgentDispatchService", "DeleteDispatch", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}}}}, options);
}
Json AgentDispatchClient::list_dispatch(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("AgentDispatchService", "ListDispatch", body, {{"video", {{"roomAdmin", true}, {"room", body.at("room")}}}}, options);
}
Json ConnectorClient::connect_twilio_call(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Connector", "ConnectTwilioCall", body, {{"video", {{"roomCreate", true}, {"room", detail::field(body, "roomName", "room_name")}}}}, options);
}
Json ConnectorClient::dial_whats_app_call(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Connector", "DialWhatsAppCall", body, {{"video", {{"roomCreate", true}, {"room", detail::field(body, "roomName", "room_name")}}}}, options);
}
Json ConnectorClient::accept_whats_app_call(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Connector", "AcceptWhatsAppCall", body, {{"video", {{"roomCreate", true}, {"room", detail::field(body, "roomName", "room_name")}}}}, options);
}
Json ConnectorClient::connect_whats_app_call(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Connector", "ConnectWhatsAppCall", body, {{"video", {{"roomCreate", true}}}}, options);
}
Json ConnectorClient::disconnect_whats_app_call(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) throw std::invalid_argument("Request must be a JSON object");
  auto body = input;
  return request("Connector", "DisconnectWhatsAppCall", body, {{"video", {{"roomCreate", true}}}}, options);
}
}

namespace livekit::sdk {
Json AgentDispatchClient::get_dispatch(const std::string& id, const std::string& room, const RequestOptions& options) const {
  auto response = list_dispatch({{"dispatchId", id}, {"room", room}}, options);
  auto found = response.find("agentDispatches");
  if (found == response.end()) found = response.find("agent_dispatches");
  if (found == response.end() || found->empty()) return nullptr;
  return found->at(0);
}
}
