#pragma once
#include <livekit/livekit.hpp>
#include <iostream>
#include <thread>

inline int api_contract(const std::string& url) {
  using namespace livekit::sdk;
  auto require = [](bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  };
  LiveKitAPI api({url, {"explicit-key", "contract-secret", {}}, true});
  Json input{{"room", "source"},
             {"roomName", "source"},
             {"destinationRoom", "target"},
             {"identity", "peer"}};
  api.room().create_room(input);
  api.room().list_rooms(input);
  api.room().delete_room(input);
  api.room().list_participants(input);
  api.room().get_participant(input);
  api.room().remove_participant(input);
  api.room().forward_participant(input);
  api.room().move_participant(input);
  api.room().mute_published_track(input);
  api.room().update_participant(input);
  api.room().update_subscriptions(input);
  api.room().update_room_metadata(input);
  api.room().send_data(input);
  api.egress().start_egress(input);
  api.egress().start_room_composite_egress(input);
  api.egress().start_participant_egress(input);
  api.egress().start_track_composite_egress(input);
  api.egress().start_track_egress(input);
  api.egress().start_web_egress(input);
  api.egress().update_layout(input);
  api.egress().update_stream(input);
  api.egress().list_egress(input);
  api.egress().stop_egress(input);
  api.ingress().create_ingress(input);
  api.ingress().update_ingress(input);
  api.ingress().list_ingress(input);
  api.ingress().delete_ingress(input);
  api.sip().create_sip_inbound_trunk(input);
  api.sip().create_sip_outbound_trunk(input);
  api.sip().update_sip_inbound_trunk(input);
  api.sip().update_sip_outbound_trunk(input);
  api.sip().list_sip_trunk(input);
  api.sip().list_sip_inbound_trunk(input);
  api.sip().list_sip_outbound_trunk(input);
  api.sip().delete_sip_trunk(input);
  api.sip().create_sip_dispatch_rule(input);
  api.sip().update_sip_dispatch_rule(input);
  api.sip().list_sip_dispatch_rule(input);
  api.sip().delete_sip_dispatch_rule(input);
  auto dial = input;
  dial["waitUntilAnswered"] = true;
  api.sip().create_sip_participant(dial);
  api.sip().transfer_sip_participant(input);
  api.agent_dispatch().create_dispatch(input);
  api.agent_dispatch().delete_dispatch(input);
  api.agent_dispatch().list_dispatch(input);
  api.connector().connect_twilio_call(input);
  api.connector().dial_whats_app_call(input);
  api.connector().accept_whats_app_call(input);
  api.connector().connect_whats_app_call(input);
  api.connector().disconnect_whats_app_call(input);
  require(api.agent_dispatch().get_dispatch("missing", "source").is_null(),
          "Missing dispatch should return null");
  LiveKitAPI ambient({url});
  ambient.room().list_rooms();
  LiveKitAPI token({url, {"ignored", "ignored", "explicit-token"}});
  token.room().list_rooms();
  bool rejected = false;
  try {
    LiveKitAPI partial({url, {"only-key", {}, {}}});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "Explicit partial credentials used environment");
  rejected = false;
  try {
    api.room().get_participant(Json{{"room", "source"}, {"identity", "error"}});
  } catch (const APIError& e) {
    rejected = e.status() == 404 && e.code() == "not_found" && e.metadata().at("detail") == "test";
  }
  require(rejected, "Structured Twirp error");
  std::stop_source cancel;
  cancel.request_stop();
  RequestOptions options;
  options.stop = cancel.get_token();
  rejected = false;
  try {
    api.room().list_rooms(Json::object(), options);
  } catch (const APIError& e) {
    rejected = e.code() == "canceled";
  }
  require(rejected, "Pre-request cancellation");
  std::stop_source during;
  options.stop = during.get_token();
  std::jthread timer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    during.request_stop();
  });
  rejected = false;
  try {
    api.room().get_participant(Json{{"room", "source"}, {"identity", "slow"}}, options);
  } catch (const APIError& e) {
    rejected = e.code() == "canceled";
  }
  require(rejected, "In-flight cancellation");
  auto jwt = api.room()
                 .create_token()
                 .set_identity("identity")
                 .set_name("name")
                 .set_metadata("meta")
                 .set_attributes({{"role", "test"}})
                 .set_kind("agent")
                 .set_valid_for(120)
                 .set_video_grant({{"roomJoin", true}, {"room", "source"}})
                 .set_sip_grant({{"call", true}})
                 .to_jwt();
  std::cout << Json{{"ok", true}, {"jwt", jwt}}.dump() << '\n';
  return 0;
}
