#include <livekit/livekit.hpp>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace sdk = livekit::sdk;
using namespace std::chrono_literals;
static void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
template<class F> void until(F condition, const char* text) {
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (!condition()) { check(std::chrono::steady_clock::now() < deadline, text); std::this_thread::sleep_for(20ms); }
}
static sdk::Json snapshot(const sdk::ParticipantInfo& p) {
  sdk::Json tracks = sdk::Json::array();
  for (const auto& t : p.tracks) tracks.push_back({{"sid", t.sid}, {"name", t.name}, {"type", static_cast<int>(t.kind)},
    {"source", static_cast<int>(t.source)}, {"muted", t.muted}, {"width", t.width}, {"height", t.height}, {"mimeType", t.mime_type}});
  return {{"sid", p.sid}, {"identity", p.identity}, {"name", p.name}, {"metadata", p.metadata}, {"tracks", tracks}};
}
int main(int argc, char** argv) {
  try {
    check(argc == 4 || argc == 5, "Usage: room_moved_integration server-url proxy-url marker-directory [rebuild]");
    const bool rebuild_on_move = argc == 5 && std::string(argv[4]) == "rebuild";
    const std::filesystem::path markers(argv[3]);
    const auto token = [](const char* identity, const char* room) {
      return sdk::make_token("move-test", "move-integration-secret-32-characters", identity,
        {{"roomJoin", true}, {"room", room}, {"canPublish", true}, {"canSubscribe", true}, {"canPublishData", true}});
    };
    std::atomic<int> source_frames{0}, target_frames{0}, target_messages{0}, moved{0}, rejoins{0}, connected{0}, disconnected{0};
    sdk::RoomOptions source_options, target_options;
    source_options.video_frame_handler = [&](const sdk::VideoFrame&) { ++source_frames; };
    target_options.video_frame_handler = [&](const sdk::VideoFrame&) { ++target_frames; };
    sdk::RoomCallback target_callback;
    target_callback.on_data_received = [&](const auto& bytes, const auto&) { if (bytes == std::vector<uint8_t>{7, 8, 9}) ++target_messages; };
    sdk::Room source, target(target_callback);
    source.join_with_token(argv[1], token("source-peer", "move-source"), source_options);
    target.join_with_token(argv[1], token("target-peer", "move-target"), target_options);
    sdk::Room* room_ptr = nullptr;
    sdk::RoomCallback cb;
    cb.on_connected = [&] { ++connected; };
    cb.on_disconnected = [&](int) { ++disconnected; };
    cb.on_reconnected = [&](const sdk::ReconnectInfo& info) { if (info.mode == sdk::ReconnectMode::rejoin) ++rejoins; };
    cb.on_room_moved = [&](const sdk::RoomMovedInfo& info) {
      check(info.previous.name == "move-source" && info.current.name == "move-target", "Wrong migration callback rooms");
      check(room_ptr->info().sid == target.info().sid, "Migration state not visible before callback");
      check(!room_ptr->participant_by_identity("source-peer"), "Old-room participant retained");
      check(room_ptr->participant_by_identity("target-peer").has_value(), "Destination snapshot missing");
      ++moved;
    };
    sdk::Room room(cb); room_ptr = &room;
    sdk::RoomOptions options; options.room = "move-source"; options.reconnect.initial_delay = 100ms;
    room.join_with_token(argv[2], token("mover", "move-source"), options);
    sdk::MediaSpec demo; demo.demo = true;
    auto original_local = room.local_participant();
    auto publication = original_local.publish_track(demo, {"moving-video"}).get();
    const auto source_id = publication.info().client_id;
    auto remote = source.local_participant().publish_track(demo, {"old-room-video"}).get();
    until([&] { auto p = room.participant_by_identity("source-peer"); return p && !p->track_publications().empty(); }, "Missing source-room track");
    auto old_remote = room.participant_by_identity("source-peer")->track_publications().front();
    until([&] { return source_frames >= 10; }, "Initial media not delivered");
    const auto destination = target.info();
    sdk::Json move{{"room", {{"sid", destination.sid}, {"name", destination.name}, {"metadata", destination.metadata}}},
      {"participant", snapshot(room.local_participant().info())},
      {"otherParticipants", sdk::Json::array({snapshot(target.local_participant().info())})}};
    { std::ofstream file(markers / "move.json"); file << move.dump(); }
    std::ofstream(markers / "move-ready") << "move";
    until([&] { return moved == 1 || disconnected; }, "RoomMoved callback timeout");
    if (disconnected) room.wait();
    if (rebuild_on_move) until([&] { return rejoins == 1 || disconnected; }, "Migration mapping mismatch did not rebuild");
    if (disconnected) room.wait();
    check(room.info().name == "move-target" && connected == 1 && rejoins == (rebuild_on_move ? 1 : 0), "Wrong migration recovery path");
    bool stale = false;
    try { original_local.set_name("stale").get(); } catch (const std::exception&) { stale = true; }
    check(stale, "Old local handle crossed room boundary");
    stale = false;
    try { old_remote.set_subscribed(true).get(); } catch (const std::exception&) { stale = true; }
    check(stale, "Old remote handle crossed room boundary");
    auto fresh = room.local_participant().track_publications().front();
    check(fresh.info().client_id == source_id, "Migration lost stable source ID");
    fresh.set_muted(false).get();
    std::this_thread::sleep_for(100ms);
    check(moved == 1 && rejoins == (rebuild_on_move ? 1 : 0), "Duplicate RoomMoved was not idempotent");
    std::ofstream(markers / "cut") << "rejoin";
    until([&] { return rejoins == (rebuild_on_move ? 2 : 1) || disconnected; }, "Destination rejoin timeout");
    if (disconnected) room.wait();
    check(room.info().name == "move-target" && room.info().sid == target.info().sid, "Rejoin returned to old room");
    until([&] { return target_frames >= 10; }, "Media missing in destination room after rejoin");
    room.local_participant().publish_data({7, 8, 9}, {true, "after-move"}).get();
    until([&] { return target_messages == 1; }, "Destination data not delivered");
    check(room.local_participant().track_publications().front().info().client_id == source_id, "Rejoin lost moving source ID");
    room.disconnect(); room.wait(); source.disconnect(); source.wait(); target.disconnect(); target.wait();
    check(connected == 1 && disconnected == 1 && moved == 1, "Incorrect lifecycle events");
    std::cout << sdk::Json{{"ok", true}, {"moved", moved.load()}, {"rejoins", rejoins.load()},
      {"destinationFrames", target_frames.load()}, {"destinationMessages", target_messages.load()},
      {"staleHandlesRejected", true}, {"stableSourceId", true}, {"mappingRebuilt", rebuild_on_move}}.dump() << '\n';
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
