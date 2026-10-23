#include "session.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>

namespace sdk = livekit::sdk;
using namespace std::chrono_literals;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static std::string env(const char* key) { auto* s = std::getenv(key); return s ? s : ""; }
template<class F> static void until(F predicate, const char* error, int seconds = 20) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!predicate()) { if (std::chrono::steady_clock::now() > end) throw std::runtime_error(error); std::this_thread::sleep_for(20ms); }
}
int main(int argc, char** argv) {
  try {
    check(argc == 2, "Expected recording directory");
    const auto url = env("LIVEKIT_URL"), key = env("LIVEKIT_API_KEY"), secret = env("LIVEKIT_API_SECRET");
    sdk::LiveKitAPI api({url, {key, secret, ""}, false});
    const std::string name = "imgui-integration";
    api.room().create_room(name);
    struct Cleanup { sdk::LiveKitAPI& api; std::string name; ~Cleanup() { try { api.room().delete_room(name); } catch (...) {} } } cleanup{api, name};
    auto options = [&](const std::string& identity) {
      sdk::RoomOptions o; o.url = url; o.room = name;
      o.token = api.room().create_token().set_identity(identity).set_name(identity)
        .set_video_grant({{"roomJoin", true}, {"room", name}, {"canPublish", true}, {"canSubscribe", true}}).to_jwt(); return o;
    };
    std::mutex received_mutex;
    std::map<std::string, uint64_t> received;
    sdk::Room first, second;
    auto one = options("Alice");
    one.video_frame_handler = [&](const sdk::VideoFrame& frame) { std::lock_guard lock(received_mutex); ++received[frame.track_id]; };
    first.join_with_token(url, one.token, one);
    sdk::MediaSpec video; video.demo = true;
    sdk::MediaSpec tone; tone.demo = true; tone.video = false;
    auto first_video = first.local_participant().publish_track(video, {"Test video"}).get();
    first.local_participant().publish_track(tone, {"Test tone"}).get();
    demo::Session viewer;
    auto view = options("Viewer"); view.record = std::filesystem::u8path(argv[1]);
    view.token = api.room().create_token().set_identity("Viewer").set_name("Viewer")
      .set_video_grant({{"roomJoin", true}, {"room", name}, {"canPublish", false}, {"canPublishData", false}, {"canSubscribe", true}}).to_jwt();
    view.audio_playout = env("LIVEKIT_DEMO_TEST_AUDIO") == "1";
    viewer.start(view);
    until([&] { auto s = viewer.snapshot(); return s.state == demo::State::Connected && s.frames.contains(first_video.sid()); }, "Initial participant/frame absent");
    check(demo::tiles(viewer.snapshot()).front().label == "Alice / Test video", "Initial typed participant mapping");
    auto two = options("Bob"); second.join_with_token(url, two.token, two);
    auto second_video = second.local_participant().publish_track(video).get();
    until([&] { auto s = viewer.snapshot(); return s.participants.size() == 3 && s.frames.contains(first_video.sid()) && s.frames.contains(second_video.sid()) && s.received_frames > 120; }, "Two simultaneous video tracks missing");
    first_video.set_muted(true).get();
    until([&] { for (const auto& t : demo::tiles(viewer.snapshot())) if (t.id == first_video.sid() && t.status == "Video muted") return true; return false; }, "Remote mute missing");
    first_video.set_muted(false).get();
    until([&] { for (const auto& t : demo::tiles(viewer.snapshot())) if (t.id == first_video.sid() && t.frame) return true; return false; }, "Remote unmute missing");
    second.local_participant().unpublish_track(second_video.sid()).get();
    until([&] { auto s = viewer.snapshot(); return !s.frames.contains(second_video.sid()); }, "Unpublished frame retained");
    second.disconnect(); second.wait();
    until([&] { return viewer.snapshot().participants.size() == 2; }, "Departed participant retained");
    viewer.stop(); until([&] { return viewer.snapshot().finished; }, "Viewer stop");
    check(viewer.snapshot().frames.empty() && viewer.snapshot().participants.empty(), "Old state after disconnect");
    view = options("Viewer");
    // Fresh join with test publishing uses LocalParticipant::publish_track futures.
    view.media = {video, video, tone}; viewer.start(view);
    until([&] { auto s = viewer.snapshot(); return s.state == demo::State::Connected && s.frames.contains(first_video.sid()); }, "Viewer reuse");
    auto local_videos = [&] {
      std::vector<sdk::TrackInfo> videos; const auto s = viewer.snapshot();
      if (auto p = s.participants.find(s.local_sid); p != s.participants.end())
        for (const auto& t : p->second.tracks) if (t.kind == sdk::TrackKind::video) videos.push_back(t);
      return videos;
    };
    auto remote_tracks = [&] { auto p = first.participant_by_identity("Viewer"); return p ? p->info().tracks : std::vector<sdk::TrackInfo>{}; };
    until([&] { return remote_tracks().size() == 3 && local_videos().size() == 2; }, "Demo multiple publications missing");
    auto initial = local_videos();
    check(initial[0].sid != initial[1].sid && initial[0].name != initial[1].name, "Video identities are not distinct");
    try {
      until([&] { std::lock_guard lock(received_mutex); return received[initial[0].sid] >= 3 && received[initial[1].sid] >= 3; }, "Same participant videos did not both decode");
    } catch (...) {
      std::lock_guard lock(received_mutex);
      for (const auto& t : remote_tracks()) std::cerr << t.sid << ' ' << t.name << " subscribed=" << t.subscribed << " frames=" << received[t.sid] << '\n';
      std::cerr << "Receiver: " << first.last_error() << " Sender: " << viewer.snapshot().error << '\n';
      throw;
    }
    check(!viewer.remove_video(first_video.sid()), "Accepted remote track removal");
    check(viewer.add_video(), "Dynamic add rejected");
    until([&] { return !viewer.snapshot().changing_video && local_videos().size() == 3 && remote_tracks().size() == 4; }, "Dynamic video missing");
    auto three = local_videos();
    until([&] { std::lock_guard lock(received_mutex); for (const auto& t : three) if (received[t.sid] < 3) return false; return true; }, "Dynamic video did not decode");
    check(viewer.remove_video(initial[0].sid), "Dynamic remove rejected");
    until([&] {
      if (viewer.snapshot().changing_video || local_videos().size() != 2) return false;
      const auto tracks = remote_tracks(); if (tracks.size() != 3) return false;
      for (const auto& t : tracks) if (t.sid == initial[0].sid) return false;
      return true;
    }, "Removed video retained");
    auto remaining = local_videos(); std::map<std::string, uint64_t> before;
    { std::lock_guard lock(received_mutex); before = received; }
    until([&] { std::lock_guard lock(received_mutex); for (const auto& t : remaining) if (received[t.sid] <= before[t.sid] + 3) return false; return true; }, "Other videos stalled after removal");
    for (const auto& t : remaining) {
      check(viewer.remove_video(t.sid), "Stop remaining video rejected");
      until([&] { return !viewer.snapshot().changing_video; }, "Stop remaining video timeout");
      check(viewer.snapshot().video_error.empty(), "Stop remaining video failed");
    }
    until([&] { const auto tracks = remote_tracks(); return tracks.size() == 1 && tracks[0].kind == sdk::TrackKind::audio && local_videos().empty(); }, "Stopping videos affected tone");
    check(viewer.add_video(), "Restart video rejected");
    until([&] { return !viewer.snapshot().changing_video && local_videos().size() == 1; }, "Restart video timeout");
    auto restarted = local_videos().front(); check(restarted.name == "Test video 4", "Reused video name");
    until([&] { std::lock_guard lock(received_mutex); return received[restarted.sid] >= 3; }, "Restarted video did not decode");
    check(viewer.add_video(), "Add before stop rejected");
    viewer.stop(); until([&] { return viewer.snapshot().finished; }, "Reused viewer stop");
    check(!viewer.snapshot().changing_video && !viewer.add_video() && !viewer.remove_video(restarted.sid), "Video operations survived disconnect");
    // Cancelling immediately must work even before join has prepared its stop source.
    for (int i = 0; i < 3; ++i) { viewer.start(view); viewer.stop(); until([&] { return viewer.snapshot().finished; }, "Immediate cancellation", 5); }
    auto wrong = options("WrongRoom"); wrong.room = "wrong-room"; viewer.start(wrong);
    until([&] { return viewer.snapshot().finished; }, "Room mismatch timeout");
    check(viewer.snapshot().state == demo::State::Error, "Room mismatch accepted");
    auto invalid = options("BadToken"); invalid.token = "invalid-token"; viewer.start(invalid);
    until([&] { return viewer.snapshot().finished; }, "Bad token timeout");
    check(viewer.snapshot().state == demo::State::Error && viewer.snapshot().error.find("invalid-token") == std::string::npos, "Token error/redaction");
    // SDK-owned worker serializes video and typed callbacks; exception cleans up.
    std::thread::id callback_thread; int frames = 0;
    sdk::RoomCallback callbacks; callbacks.on_connected = [&] { callback_thread = std::this_thread::get_id(); };
    callbacks.on_track_subscribed = [&](const sdk::TrackPublication&, const sdk::RemoteParticipant&) { callback_thread = std::this_thread::get_id(); };
    sdk::Room throwing(callbacks); auto fail = options("Throwing");
    fail.video_frame_handler = [&](const sdk::VideoFrame&) {
      check(callback_thread == std::this_thread::get_id(), "Video callback not on session thread");
      ++frames; throw std::runtime_error("expected video exception");
    };
    bool caught = false;
    try { throwing.join_with_token(url, fail.token, fail); throwing.wait(); }
    catch (const std::exception& e) { caught = std::string(e.what()) == "expected video exception"; }
    check(caught && frames == 1, "Video callback exception propagation");
    throwing.disconnect();
    std::stop_source stopped; stopped.request_stop();
    fail.video_frame_handler = {}; caught = false;
    try { throwing.join_with_token(url, fail.token, fail, stopped.get_token()); } catch (...) { caught = true; }
    check(caught, "Pre-cancelled join succeeded");
    // Server-side removal must become an actionable Error with cleared media.
    view.media.clear(); viewer.start(view);
    until([&] { return viewer.snapshot().state == demo::State::Connected; }, "Final connect");
    api.room().remove_participant(name, "Viewer");
    until([&] { return viewer.snapshot().finished; }, "Server removal not observed");
    check(viewer.snapshot().state == demo::State::Error && viewer.snapshot().frames.empty(), "Server removal state");
    first.disconnect(); first.wait();
    std::cout << sdk::Json{{"ok", true}, {"simultaneousVideos", 2}, {"videosPerParticipant", 3}, {"dynamicVideoPublishing", true}, {"typedAPI", true}, {"reusedSessions", 2}, {"immediateCancellations", 3}}.dump() << '\n';
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
