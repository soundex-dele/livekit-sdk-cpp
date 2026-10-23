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
    sdk::Room first, second;
    auto one = options("Alice"); first.join_with_token(url, one.token, one);
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
    check(demo::tiles(viewer.snapshot()).front().label == "Alice", "Initial typed participant mapping");
    auto two = options("Bob"); second.join_with_token(url, two.token, two);
    auto second_video = second.local_participant().publish_track(video).get();
    until([&] { auto s = viewer.snapshot(); return s.participants.size() == 3 && s.frames.contains(first_video.sid()) && s.frames.contains(second_video.sid()) && s.received_frames > 120; }, "Two simultaneous video tracks missing");
    first_video.set_muted(true).get();
    until([&] { for (const auto& t : demo::tiles(viewer.snapshot())) if (t.label == "Alice" && t.status == "Video muted") return true; return false; }, "Remote mute missing");
    first_video.set_muted(false).get();
    until([&] { for (const auto& t : demo::tiles(viewer.snapshot())) if (t.label == "Alice" && t.frame) return true; return false; }, "Remote unmute missing");
    second.local_participant().unpublish_track(second_video.sid()).get();
    until([&] { auto s = viewer.snapshot(); return !s.frames.contains(second_video.sid()); }, "Unpublished frame retained");
    second.disconnect(); second.wait();
    until([&] { return viewer.snapshot().participants.size() == 2; }, "Departed participant retained");
    viewer.stop(); until([&] { return viewer.snapshot().finished; }, "Viewer stop");
    check(viewer.snapshot().frames.empty() && viewer.snapshot().participants.empty(), "Old state after disconnect");
    view = options("Viewer");
    // Fresh join with test publishing uses LocalParticipant::publish_track futures.
    view.media = {video}; viewer.start(view);
    until([&] { auto s = viewer.snapshot(); return s.state == demo::State::Connected && s.frames.contains(first_video.sid()); }, "Viewer reuse");
    until([&] { auto p = first.participant_by_identity("Viewer"); return p && !p->track_publications().empty(); }, "Demo test video publication");
    viewer.stop(); until([&] { return viewer.snapshot().finished; }, "Reused viewer stop");
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
    std::cout << sdk::Json{{"ok", true}, {"simultaneousVideos", 2}, {"typedAPI", true}, {"reusedSessions", 2}, {"immediateCancellations", 3}}.dump() << '\n';
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
