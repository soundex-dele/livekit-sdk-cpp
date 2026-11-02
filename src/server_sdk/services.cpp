#include "livekit/services.hpp"
#include "livekit/room_service.hpp"
#include "server_sdk/api_transport.hpp"
#include <cmath>
#include <charconv>
#include <limits>

namespace livekit::sdk {
namespace {
Result<RequestOptions> dial_options(Json& body, RequestOptions options) {
  const char* key = body.contains("ringing_timeout") ? "ringing_timeout" : "ringingTimeout";
  if (!body.contains(key) || body[key].is_null()) {
    body[key] = "30s";
  }
  if (!body[key].is_string()) {
    return Result<RequestOptions>::failure(
        detail::invalid_request("ringingTimeout must be a protobuf duration"));
  }
  auto text = body[key].get<std::string>();
  if (text.empty() || text.back() != 's') {
    return Result<RequestOptions>::failure(
        detail::invalid_request("ringingTimeout must be a protobuf duration (e.g. 30s)"));
  }
  text.pop_back();
  double ring = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), ring);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(ring) ||
      ring < 0 || ring > std::numeric_limits<int>::max() - 3) {
    return Result<RequestOptions>::failure(detail::invalid_request("Invalid ringingTimeout"));
  }
  options.timeout = std::max(options.timeout, static_cast<int>(std::ceil(ring + 2)));
  return Result<RequestOptions>::success(options);
}
}  // namespace

Result<Json> RoomServiceClient::create_room(const Json& input,
                                            const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("RoomService", "CreateRoom", body, {{"video", {{"roomCreate", true}}}}, options);
}

Result<Json> RoomServiceClient::list_rooms(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("RoomService", "ListRooms", body, {{"video", {{"roomList", true}}}}, options);
}

Result<Json> RoomServiceClient::delete_room(const Json& input,
                                            const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("RoomService", "DeleteRoom", body, {{"video", {{"roomCreate", true}}}}, options);
}

Result<Json> RoomServiceClient::list_participants(const Json& input,
                                                  const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  return request("RoomService", "ListParticipants", body,
                 {{"video", {{"roomAdmin", true}, {"room", *room}}}}, options);
}

Result<Json> RoomServiceClient::get_participant(const Json& input,
                                                const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  return request("RoomService", "GetParticipant", body,
                 {{"video", {{"roomAdmin", true}, {"room", *room}}}}, options);
}

Result<Json> RoomServiceClient::remove_participant(const Json& input,
                                                   const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  return request("RoomService", "RemoveParticipant", body,
                 {{"video", {{"roomAdmin", true}, {"room", *room}}}}, options);
}

Result<Json> RoomServiceClient::forward_participant(const Json& input,
                                                    const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  auto destinationRoom = detail::field(body, "destinationRoom", "destination_room");
  if (!destinationRoom) {
    return Result<Json>::failure(destinationRoom.failure_record());
  }
  return request(
      "RoomService", "ForwardParticipant", body,
      {{"video", {{"roomAdmin", true}, {"room", *room}, {"destinationRoom", *destinationRoom}}}},
      options);
}

Result<Json> RoomServiceClient::move_participant(const Json& input,
                                                 const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  auto destinationRoom = detail::field(body, "destinationRoom", "destination_room");
  if (!destinationRoom) {
    return Result<Json>::failure(destinationRoom.failure_record());
  }
  return request(
      "RoomService", "MoveParticipant", body,
      {{"video", {{"roomAdmin", true}, {"room", *room}, {"destinationRoom", *destinationRoom}}}},
      options);
}

Result<Json> RoomServiceClient::mute_published_track(const Json& input,
                                                     const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  return request("RoomService", "MutePublishedTrack", body,
                 {{"video", {{"roomAdmin", true}, {"room", *room}}}}, options);
}

Result<Json> RoomServiceClient::update_participant(const Json& input,
                                                   const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  return request("RoomService", "UpdateParticipant", body,
                 {{"video", {{"roomAdmin", true}, {"room", *room}}}}, options);
}

Result<Json> RoomServiceClient::update_subscriptions(const Json& input,
                                                     const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  return request("RoomService", "UpdateSubscriptions", body,
                 {{"video", {{"roomAdmin", true}, {"room", *room}}}}, options);
}

Result<Json> RoomServiceClient::update_room_metadata(const Json& input,
                                                     const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  return request("RoomService", "UpdateRoomMetadata", body,
                 {{"video", {{"roomAdmin", true}, {"room", *room}}}}, options);
}

Result<Json> RoomServiceClient::send_data(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  auto nonce = detail::random_base64(16);
  if (!nonce) {
    return Result<Json>::failure(nonce.failure_record());
  }
  body["nonce"] = std::move(*nonce);
  return request("RoomService", "SendData", body,
                 {{"video", {{"roomAdmin", true}, {"room", *room}}}}, options);
}

Result<Json> EgressClient::start_egress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Egress", "StartEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}

Result<Json> EgressClient::start_room_composite_egress(const Json& input,
                                                       const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Egress", "StartRoomCompositeEgress", body, {{"video", {{"roomRecord", true}}}},
                 options);
}

Result<Json> EgressClient::start_participant_egress(const Json& input,
                                                    const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Egress", "StartParticipantEgress", body, {{"video", {{"roomRecord", true}}}},
                 options);
}

Result<Json> EgressClient::start_track_composite_egress(const Json& input,
                                                        const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Egress", "StartTrackCompositeEgress", body, {{"video", {{"roomRecord", true}}}},
                 options);
}

Result<Json> EgressClient::start_track_egress(const Json& input,
                                              const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Egress", "StartTrackEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}

Result<Json> EgressClient::start_web_egress(const Json& input,
                                            const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Egress", "StartWebEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}

Result<Json> EgressClient::update_layout(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Egress", "UpdateLayout", body, {{"video", {{"roomRecord", true}}}}, options);
}

Result<Json> EgressClient::update_stream(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Egress", "UpdateStream", body, {{"video", {{"roomRecord", true}}}}, options);
}

Result<Json> EgressClient::list_egress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Egress", "ListEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}

Result<Json> EgressClient::stop_egress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Egress", "StopEgress", body, {{"video", {{"roomRecord", true}}}}, options);
}

Result<Json> IngressClient::create_ingress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Ingress", "CreateIngress", body, {{"video", {{"ingressAdmin", true}}}}, options);
}

Result<Json> IngressClient::update_ingress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Ingress", "UpdateIngress", body, {{"video", {{"ingressAdmin", true}}}}, options);
}

Result<Json> IngressClient::list_ingress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Ingress", "ListIngress", body, {{"video", {{"ingressAdmin", true}}}}, options);
}

Result<Json> IngressClient::delete_ingress(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Ingress", "DeleteIngress", body, {{"video", {{"ingressAdmin", true}}}}, options);
}

Result<Json> SIPClient::create_sip_inbound_trunk(const Json& input,
                                                 const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("SIP", "CreateSIPInboundTrunk", body, {{"sip", {{"admin", true}}}}, options);
}

Result<Json> SIPClient::create_sip_outbound_trunk(const Json& input,
                                                  const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("SIP", "CreateSIPOutboundTrunk", body, {{"sip", {{"admin", true}}}}, options);
}

Result<Json> SIPClient::update_sip_inbound_trunk(const Json& input,
                                                 const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("SIP", "UpdateSIPInboundTrunk", body, {{"sip", {{"admin", true}}}}, options);
}

Result<Json> SIPClient::update_sip_outbound_trunk(const Json& input,
                                                  const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("SIP", "UpdateSIPOutboundTrunk", body, {{"sip", {{"admin", true}}}}, options);
}

Result<Json> SIPClient::list_sip_trunk(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("SIP", "ListSIPTrunk", body, {{"sip", {{"admin", true}}}}, options);
}

Result<Json> SIPClient::list_sip_inbound_trunk(const Json& input,
                                               const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("SIP", "ListSIPInboundTrunk", body, {{"sip", {{"admin", true}}}}, options);
}

Result<Json> SIPClient::list_sip_outbound_trunk(const Json& input,
                                                const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("SIP", "ListSIPOutboundTrunk", body, {{"sip", {{"admin", true}}}}, options);
}

Result<Json> SIPClient::delete_sip_trunk(const Json& input, const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("SIP", "DeleteSIPTrunk", body, {{"sip", {{"admin", true}}}}, options);
}

Result<Json> SIPClient::create_sip_dispatch_rule(const Json& input,
                                                 const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("SIP", "CreateSIPDispatchRule", body, {{"sip", {{"admin", true}}}}, options);
}

Result<Json> SIPClient::update_sip_dispatch_rule(const Json& input,
                                                 const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("SIP", "UpdateSIPDispatchRule", body, {{"sip", {{"admin", true}}}}, options);
}

Result<Json> SIPClient::list_sip_dispatch_rule(const Json& input,
                                               const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("SIP", "ListSIPDispatchRule", body, {{"sip", {{"admin", true}}}}, options);
}

Result<Json> SIPClient::delete_sip_dispatch_rule(const Json& input,
                                                 const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("SIP", "DeleteSIPDispatchRule", body, {{"sip", {{"admin", true}}}}, options);
}

Result<Json> SIPClient::create_sip_participant(const Json& input,
                                               const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto effective = options;
  const char* wait_key =
      body.contains("waitUntilAnswered") ? "waitUntilAnswered" : "wait_until_answered";
  const auto wait = body.find(wait_key);
  if (wait != body.end() && !wait->is_boolean()) {
    return Result<Json>::failure(detail::invalid_request("waitUntilAnswered must be a boolean"));
  }
  if (wait != body.end() && wait->get<bool>()) {
    auto dial = dial_options(body, options);
    if (!dial) {
      return Result<Json>::failure(dial.failure_record());
    }
    effective = *dial;
  }
  return request("SIP", "CreateSIPParticipant", body, {{"sip", {{"call", true}}}}, effective);
}

Result<Json> SIPClient::transfer_sip_participant(const Json& input,
                                                 const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto roomName = detail::field(body, "roomName", "room_name");
  if (!roomName) {
    return Result<Json>::failure(roomName.failure_record());
  }
  auto effective = dial_options(body, options);
  if (!effective) {
    return Result<Json>::failure(effective.failure_record());
  }
  return request("SIP", "TransferSIPParticipant", body,
                 {{"sip", {{"call", true}}}, {"video", {{"roomAdmin", true}, {"room", *roomName}}}},
                 *effective);
}

Result<Json> AgentDispatchClient::create_dispatch(const Json& input,
                                                  const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  return request("AgentDispatchService", "CreateDispatch", body,
                 {{"video", {{"roomAdmin", true}, {"room", *room}}}}, options);
}

Result<Json> AgentDispatchClient::delete_dispatch(const Json& input,
                                                  const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  return request("AgentDispatchService", "DeleteDispatch", body,
                 {{"video", {{"roomAdmin", true}, {"room", *room}}}}, options);
}

Result<Json> AgentDispatchClient::list_dispatch(const Json& input,
                                                const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto room = detail::field(body, "room");
  if (!room) {
    return Result<Json>::failure(room.failure_record());
  }
  return request("AgentDispatchService", "ListDispatch", body,
                 {{"video", {{"roomAdmin", true}, {"room", *room}}}}, options);
}

Result<Json> ConnectorClient::connect_twilio_call(const Json& input,
                                                  const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto roomName = detail::field(body, "roomName", "room_name");
  if (!roomName) {
    return Result<Json>::failure(roomName.failure_record());
  }
  return request("Connector", "ConnectTwilioCall", body,
                 {{"video", {{"roomCreate", true}, {"room", *roomName}}}}, options);
}

Result<Json> ConnectorClient::dial_whats_app_call(const Json& input,
                                                  const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto roomName = detail::field(body, "roomName", "room_name");
  if (!roomName) {
    return Result<Json>::failure(roomName.failure_record());
  }
  return request("Connector", "DialWhatsAppCall", body,
                 {{"video", {{"roomCreate", true}, {"room", *roomName}}}}, options);
}

Result<Json> ConnectorClient::accept_whats_app_call(const Json& input,
                                                    const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  auto roomName = detail::field(body, "roomName", "room_name");
  if (!roomName) {
    return Result<Json>::failure(roomName.failure_record());
  }
  return request("Connector", "AcceptWhatsAppCall", body,
                 {{"video", {{"roomCreate", true}, {"room", *roomName}}}}, options);
}

Result<Json> ConnectorClient::connect_whats_app_call(const Json& input,
                                                     const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Connector", "ConnectWhatsAppCall", body, {{"video", {{"roomCreate", true}}}},
                 options);
}

Result<Json> ConnectorClient::disconnect_whats_app_call(const Json& input,
                                                        const RequestOptions& options) const {
  if (!input.is_object()) {
    return Result<Json>::failure(detail::invalid_request("Request must be a JSON object"));
  }
  auto body = input;
  return request("Connector", "DisconnectWhatsAppCall", body, {{"video", {{"roomCreate", true}}}},
                 options);
}
}  // namespace livekit::sdk

namespace livekit::sdk {
Result<Json> AgentDispatchClient::get_dispatch(const std::string& id, const std::string& room,
                                               const RequestOptions& options) const {
  auto response = list_dispatch({{"dispatchId", id}, {"room", room}}, options);
  if (!response) {
    return response;
  }
  if (!response->is_object()) {
    return Result<Json>::failure(make_failure(ErrorDomain::protocol, ErrorCode::malformed_message,
                                              "Dispatch response must be a JSON object"));
  }
  auto found = response->find("agentDispatches");
  if (found == response->end()) {
    found = response->find("agent_dispatches");
  }
  if (found == response->end() || found->is_null()) {
    return Result<Json>::success(nullptr);
  }
  if (!found->is_array()) {
    return Result<Json>::failure(make_failure(ErrorDomain::protocol, ErrorCode::malformed_message,
                                              "Dispatch response must contain an array"));
  }
  return Result<Json>::success(found->empty() ? Json(nullptr) : (*found)[0]);
}
}  // namespace livekit::sdk
