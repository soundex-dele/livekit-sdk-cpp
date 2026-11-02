#pragma once
#include "livekit/compat/thread.hpp"
#include <livekit/api.hpp>
#include <iostream>
#include <thread>
#include <cstdlib>

inline int api_contract(const std::string& url) {
  using namespace livekit::sdk;
  auto require = [](bool ok, const char* message) {
    if (!ok) {
      std::cerr << message << "\n";
      std::exit(1);
    }
  };
  LiveKitAPI api({url, {"explicit-key", "contract-secret", {}}, true});
  Json input{{"room", "source"},
             {"roomName", "source"},
             {"destinationRoom", "target"},
             {"identity", "peer"}};
  require(bool(api.room().create_room(input)), "create_room failed");
  require(bool(api.room().list_rooms(input)), "list_rooms failed");
  require(bool(api.room().delete_room(input)), "delete_room failed");
  require(bool(api.room().list_participants(input)), "list_participants failed");
  require(bool(api.room().get_participant(input)), "get_participant failed");
  require(bool(api.room().remove_participant(input)), "remove_participant failed");
  require(bool(api.room().forward_participant(input)), "forward_participant failed");
  require(bool(api.room().move_participant(input)), "move_participant failed");
  require(bool(api.room().mute_published_track(input)), "mute_published_track failed");
  require(bool(api.room().update_participant(input)), "update_participant failed");
  require(bool(api.room().update_subscriptions(input)), "update_subscriptions failed");
  require(bool(api.room().update_room_metadata(input)), "update_room_metadata failed");
  require(bool(api.room().send_data(input)), "send_data failed");
  require(bool(api.egress().start_egress(input)), "start_egress failed");
  require(bool(api.egress().start_room_composite_egress(input)),
          "start_room_composite_egress failed");
  require(bool(api.egress().start_participant_egress(input)), "start_participant_egress failed");
  require(bool(api.egress().start_track_composite_egress(input)),
          "start_track_composite_egress failed");
  require(bool(api.egress().start_track_egress(input)), "start_track_egress failed");
  require(bool(api.egress().start_web_egress(input)), "start_web_egress failed");
  require(bool(api.egress().update_layout(input)), "update_layout failed");
  require(bool(api.egress().update_stream(input)), "update_stream failed");
  require(bool(api.egress().list_egress(input)), "list_egress failed");
  require(bool(api.egress().stop_egress(input)), "stop_egress failed");
  require(bool(api.ingress().create_ingress(input)), "create_ingress failed");
  require(bool(api.ingress().update_ingress(input)), "update_ingress failed");
  require(bool(api.ingress().list_ingress(input)), "list_ingress failed");
  require(bool(api.ingress().delete_ingress(input)), "delete_ingress failed");
  require(bool(api.sip().create_sip_inbound_trunk(input)), "create_sip_inbound_trunk failed");
  require(bool(api.sip().create_sip_outbound_trunk(input)), "create_sip_outbound_trunk failed");
  require(bool(api.sip().update_sip_inbound_trunk(input)), "update_sip_inbound_trunk failed");
  require(bool(api.sip().update_sip_outbound_trunk(input)), "update_sip_outbound_trunk failed");
  require(bool(api.sip().list_sip_trunk(input)), "list_sip_trunk failed");
  require(bool(api.sip().list_sip_inbound_trunk(input)), "list_sip_inbound_trunk failed");
  require(bool(api.sip().list_sip_outbound_trunk(input)), "list_sip_outbound_trunk failed");
  require(bool(api.sip().delete_sip_trunk(input)), "delete_sip_trunk failed");
  require(bool(api.sip().create_sip_dispatch_rule(input)), "create_sip_dispatch_rule failed");
  require(bool(api.sip().update_sip_dispatch_rule(input)), "update_sip_dispatch_rule failed");
  require(bool(api.sip().list_sip_dispatch_rule(input)), "list_sip_dispatch_rule failed");
  require(bool(api.sip().delete_sip_dispatch_rule(input)), "delete_sip_dispatch_rule failed");
  auto dial = input;
  dial["waitUntilAnswered"] = true;
  require(bool(api.sip().create_sip_participant(dial)), "create_sip_participant failed");
  require(bool(api.sip().transfer_sip_participant(input)), "transfer_sip_participant failed");
  require(bool(api.agent_dispatch().create_dispatch(input)), "create_dispatch failed");
  require(bool(api.agent_dispatch().delete_dispatch(input)), "delete_dispatch failed");
  require(bool(api.agent_dispatch().list_dispatch(input)), "list_dispatch failed");
  require(bool(api.connector().connect_twilio_call(input)), "connect_twilio_call failed");
  require(bool(api.connector().dial_whats_app_call(input)), "dial_whats_app_call failed");
  require(bool(api.connector().accept_whats_app_call(input)), "accept_whats_app_call failed");
  require(bool(api.connector().connect_whats_app_call(input)), "connect_whats_app_call failed");
  require(bool(api.connector().disconnect_whats_app_call(input)),
          "disconnect_whats_app_call failed");
  auto missing = api.agent_dispatch().get_dispatch("missing", "source");
  require(missing && missing->is_null(), "Missing dispatch should return null");
  LiveKitAPI ambient({url});
  require(bool(ambient.room().list_rooms()), "list_rooms failed");
  LiveKitAPI token({url, {"ignored", "ignored", "explicit-token"}});
  require(bool(token.room().list_rooms()), "list_rooms failed");
  LiveKitAPI partial({url, {"only-key", {}, {}}});
  require(!partial.room().status(), "Explicit partial credentials used environment");
  auto failure = api.room().get_participant(Json{{"room", "source"}, {"identity", "error"}});
  require(!failure && failure.error().backend_reason == 404 &&
              failure.error().backend_code == "not_found" &&
              Json::parse(failure.error().backend_metadata, nullptr, false)["detail"] == "test",
          "Structured Twirp error");
  livekit::compat::stop_source cancel;
  cancel.request_stop();
  RequestOptions options;
  options.stop = cancel.get_token();
  auto cancelled = api.room().list_rooms(Json::object(), options);
  require(!cancelled && cancelled.error().code == ErrorCode::cancelled, "Pre-request cancellation");
  livekit::compat::stop_source during;
  options.stop = during.get_token();
  livekit::compat::jthread timer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    during.request_stop();
  });
  auto slow = api.room().get_participant(Json{{"room", "source"}, {"identity", "slow"}}, options);
  require(!slow && slow.error().code == ErrorCode::cancelled, "In-flight cancellation");
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
  require(bool(jwt), "Token creation failed");
  std::cout << Json{{"ok", true}, {"jwt", *jwt}}.dump() << '\n';
  return 0;
}
