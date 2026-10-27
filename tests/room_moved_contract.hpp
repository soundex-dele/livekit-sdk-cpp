#pragma once
#include "engines/rtc/session.hpp"

inline void room_moved_contract() {
  using namespace livekit::sdk;
  auto require = [](bool ok) { if (!ok) throw std::runtime_error("RoomMoved state contract failed"); };
  auto state = std::make_shared<detail::RoomState>();
  state->running = true; state->generation = 7; state->connection = ConnectionState::connected;
  state->room = {"RM_old", "old", "old-metadata", false};
  state->local.sid = "PA_local"; state->local.identity = "local";
  TrackInfo local_track; local_track.sid = "TR_local"; local_track.client_id = "video-1";
  state->local.tracks = {local_track}; state->recovery.local_ids["TR_local"] = "video-1";
  ParticipantInfo previous; previous.sid = "PA_old"; previous.identity = "peer";
  TrackInfo remote_track; remote_track.sid = "TR_old"; remote_track.subscribed = true;
  previous.tracks = {remote_track}; state->remote["peer"] = previous; state->subscriptions["TR_old"] = true;
  state->subscriptions["TR_new"] = true; // native OnTrack arrived before destination roster
  LocalParticipant old_local(state, 7, state->local, true);
  TrackPublication old_remote(state, 7, "peer", remote_track, false);
  auto queued = old_local.set_metadata("old-room-command");
  int removed = 0, added = 0, moved = 0, unpublished = 0, unsubscribed = 0, subscribed = 0;
  state->callbacks.on_participant_disconnected = [&](const RemoteParticipant& p) {
    require(p.sid() == "PA_old" && p.info().disconnect_reason == 8); ++removed;
  };
  state->callbacks.on_track_unpublished = [&](const auto&, const auto&) { ++unpublished; };
  state->callbacks.on_track_unsubscribed = [&](const auto&, const auto&) { ++unsubscribed; };
  state->callbacks.on_track_subscribed = [&](const TrackPublication& track, const RemoteParticipant& p) {
    require(track.sid() == "TR_new" && track.info().subscribed && p.sid() == "PA_new"); ++subscribed;
  };
  state->callbacks.on_participant_connected = [&](const RemoteParticipant& p) {
    require(p.sid() == "PA_new" && state->room.sid == "RM_new"); ++added;
  };
  state->callbacks.on_room_moved = [&](const RoomMovedInfo& info) {
    require(info.previous.sid == "RM_old" && info.current.name == "new" && state->room.sid == "RM_new");
    ++moved;
    // The new callback's command must survive the old queue's cancellation.
    LocalParticipant(state, state->generation, state->local, true).set_name("new-room-command");
  };
  Json snapshot{{"event", "room-moved"}, {"roomInfo", {{"sid", "RM_new"}, {"name", "new"}, {"metadata", "new-metadata"}, {"recording", true}}},
    {"localParticipant", {{"sid", "PA_local"}, {"identity", "local"}, {"tracks", Json::array({{{"sid", "TR_local"}}})}}},
    {"participants", Json::array({{{"sid", "PA_new"}, {"identity", "peer"}, {"tracks", Json::array({{{"sid", "TR_new"}}})}}})},
    {"transportRetained", true}};
  auto invalid = snapshot;
  invalid["participants"].push_back(invalid["participants"].at(0));
  bool failed = false;
  try { detail::process_event(state, invalid); } catch (const SDKError& e) { failed = e.code() == ErrorCode::malformed_message; }
  require(failed && state->generation == 7 && state->room.sid == "RM_old" && moved == 0);
  detail::process_event(state, snapshot);
  require(state->generation == 8 && state->connection == ConnectionState::connected && state->room.recording);
  require(state->remote.size() == 1 && state->remote.at("peer").sid == "PA_new" && state->subscriptions.size() == 1 && state->subscriptions.at("TR_new"));
  require(state->local.tracks.at(0).client_id == "video-1" && state->recovery.local_snapshot->sid == "PA_local");
  require(moved == 1 && removed == 1 && added == 1 && unpublished == 1 && unsubscribed == 1 && subscribed == 1);
  failed = false;
  try { queued.get(); } catch (const SDKError& e) { failed = e.code() == ErrorCode::session_closed; }
  require(failed);
  failed = false;
  try { old_local.set_name("must-not-cross-room").get(); } catch (const std::exception&) { failed = true; }
  require(failed);
  failed = false;
  try { old_remote.set_subscribed(true).get(); } catch (const std::exception&) { failed = true; }
  require(failed);
  auto commands = state->take_commands();
  require(commands.size() == 1 && commands.front()->args.at("name") == "new-room-command");
  commands.front()->resolve();
}
