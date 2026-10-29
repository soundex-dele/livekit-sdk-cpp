#include "session.hpp"
#include <livekit/api.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>

namespace sdk = livekit::sdk;
using namespace std::chrono_literals;

static void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

static std::string env(const char* key) {
  auto* s = std::getenv(key);
  return s ? s : "";
}

template <class F>
static void until(F predicate, const char* error, int seconds = 20) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() > end)
      throw std::runtime_error(error);
    std::this_thread::sleep_for(20ms);
  }
}

static std::vector<demo::rtc::PublicationInfo> tracks(const demo::Session& session,
                                                      const std::string& identity = {},
                                                      bool video_only = false) {
  const auto s = session.snapshot();
  if (s.state == demo::State::Error)
    throw std::runtime_error("Conference session: " + s.error);
  for (const auto& [sid, participant] : s.participants)
    if (identity.empty() ? sid == s.local_sid : participant.identity == identity) {
      auto result = participant.tracks;
      if (video_only)
        std::erase_if(result, [](const auto& track) {
          return track.kind != demo::rtc::PublicationKind::video;
        });
      return result;
    }
  return {};
}

static void stopped(demo::Session& session) {
  session.stop();
  until([&] { return session.snapshot().finished; }, "Session stop");
}

int main(int argc, char** argv) {
  try {
    check(argc == 2, "Expected recording directory");
    const auto url = env("LIVEKIT_URL"), key = env("LIVEKIT_API_KEY"),
               secret = env("LIVEKIT_API_SECRET");
    sdk::LiveKitAPI api({url, {key, secret, ""}, false});
    const std::string name = "imgui-integration";
    std::filesystem::create_directories(std::filesystem::u8path(argv[1]));
    std::ofstream stats_log(std::filesystem::u8path(argv[1]) / "dashboard-stats.jsonl");
    api.room().create_room(name);

    struct Cleanup {
      sdk::LiveKitAPI& api;
      std::string name;

      ~Cleanup() {
        try {
          api.room().delete_room(name);
        } catch (...) {
        }
      }
    } cleanup{api, name};

    auto options = [&](const std::string& identity) {
      demo::SessionOptions o;
      o.connection.url = url;
      o.room = name;
      if (identity == "Viewer")
        o.log_sink = [&](const sdk::Json& event) { stats_log << event.dump() << '\n'; };
      o.connection.token = api.room()
                               .create_token()
                               .set_identity(identity)
                               .set_name(identity)
                               .set_video_grant({{"roomJoin", true},
                                                 {"room", name},
                                                 {"canPublish", true},
                                                 {"canSubscribe", true}})
                               .to_jwt();
      return o;
    };
    sdk::MediaSpec video;
    video.demo = true;
    sdk::MediaSpec tone;
    tone.demo = true;
    tone.video = false;
    demo::Session first, second, viewer;
    auto one = options("Alice");
    one.media = {video, tone};
    first.start(one);
    until([&] { return tracks(first, {}, true).size() == 1; }, "Initial publication");
    const auto first_video = tracks(first, {}, true).front().sid;
    auto view = options("Viewer");
    view.record = std::filesystem::u8path(argv[1]);
    view.connection.token = api.room()
                                .create_token()
                                .set_identity("Viewer")
                                .set_name("Viewer")
                                .set_video_grant({{"roomJoin", true},
                                                  {"room", name},
                                                  {"canPublish", false},
                                                  {"canPublishData", false},
                                                  {"canSubscribe", true}})
                                .to_jwt();
    view.audio_playout = env("LIVEKIT_DEMO_TEST_AUDIO") == "1";
    viewer.start(view);
    until(
        [&] {
          const auto s = viewer.snapshot();
          return s.state == demo::State::Connected && s.frames.contains(first_video);
        },
        "Initial participant/frame absent");
    check(demo::tiles(viewer.snapshot()).front().label == "Alice / Test video 1",
          "Initial typed participant mapping");
    auto two = options("Bob");
    two.media = {video};
    second.start(two);
    until([&] { return tracks(second, {}, true).size() == 1; }, "Second publication");
    const auto second_video = tracks(second, {}, true).front().sid;
    until(
        [&] {
          const auto s = viewer.snapshot();
          return s.participants.size() == 3 && s.frames.contains(first_video) &&
                 s.frames.contains(second_video) && s.received_frames > 120;
        },
        "Two simultaneous video tracks missing");
    using Stage = demo::QualitySnapshot::Stage;
    using Bitrate = demo::QualitySnapshot::Bitrate;
    until(
        [&] {
          const auto q = viewer.snapshot().quality;
          for (const auto& sid : {first_video, second_video})
            if (q.fps(Stage::receive, sid).value_or(0) <= 0 ||
                q.fps(Stage::decode, sid).value_or(0) <= 0 ||
                q.bitrate(Bitrate::rtp, sid, false).value_or(0) <= 0)
              return false;
          return true;
        },
        "Dashboard per-track receive/decode FPS or bitrate missing");
    api.room().mute_published_track(name, "Alice", first_video, true);
    until(
        [&] {
          for (const auto& tile : demo::tiles(viewer.snapshot()))
            if (tile.id == first_video && tile.status == "Video muted")
              return true;
          return false;
        },
        "Remote mute missing");
    api.room().mute_published_track(name, "Alice", first_video, false);
    until(
        [&] {
          for (const auto& tile : demo::tiles(viewer.snapshot()))
            if (tile.id == first_video && tile.frame)
              return true;
          return false;
        },
        "Remote unmute missing");
    check(second.remove_video(second_video), "Remove second video rejected");
    until([&] { return !viewer.snapshot().frames.contains(second_video); },
          "Unpublished frame retained");
    stopped(second);
    until([&] { return viewer.snapshot().participants.size() == 2; },
          "Departed participant retained");
    stopped(viewer);
    check(viewer.snapshot().frames.empty() && viewer.snapshot().participants.empty() &&
              viewer.snapshot().quality.nodes.empty(),
          "Old state after disconnect");
    bool wav = false, y4m = false;
    for (const auto& entry : std::filesystem::directory_iterator(view.record)) {
      if (entry.path().extension() == ".wav" && entry.file_size() > 48000)
        wav = true;
      if (entry.path().extension() == ".y4m" && entry.file_size() > 640 * 360 * 3 / 2 * 20)
        y4m = true;
    }
    check(wav && y4m, "Graph recording outputs absent");
    view = options("Viewer");
    view.media = {video, video, tone};
    viewer.start(view);
    try {
      until(
          [&] {
            return tracks(first, "Viewer").size() == 3 && tracks(viewer, {}, true).size() == 2;
          },
          "Multiple publications after reuse missing");
    } catch (...) {
      std::cerr << "Remote publications=" << tracks(first, "Viewer").size()
                << " local videos=" << tracks(viewer, {}, true).size() << '\n';
      throw;
    }
    until(
        [&] {
          const auto q = viewer.snapshot().quality;
          for (const auto& track : tracks(viewer, {}, true))
            for (auto stage : {Stage::capture, Stage::encode, Stage::send})
              if (q.fps(stage, track.sid).value_or(0) <= 0 ||
                  q.bitrate(Bitrate::rtp, track.sid, true).value_or(0) <= 0)
                return false;
          return q.fps(Stage::receive, first_video).value_or(0) > 0 &&
                 q.fps(Stage::decode, first_video).value_or(0) > 0;
        },
        "Dashboard independent per-track stage statistics missing");
    auto initial = tracks(viewer, {}, true);
    check(initial[0].sid != initial[1].sid && initial[0].name != initial[1].name,
          "Distinct publication identities");
    check(viewer.add_video(), "Dynamic add rejected");
    until(
        [&] {
          return !viewer.snapshot().changing_video && tracks(viewer, {}, true).size() == 3 &&
                 tracks(first, "Viewer").size() == 4;
        },
        "Dynamic publication missing");
    const auto three = tracks(viewer, {}, true);
    until(
        [&] {
          const auto s = first.snapshot();
          for (const auto& track : three)
            if (!s.frames.contains(track.sid))
              return false;
          return true;
        },
        "Dynamic streams did not all decode");
    check(viewer.remove_video(initial[0].sid), "Dynamic remove rejected");
    until(
        [&] {
          return !viewer.snapshot().changing_video && tracks(viewer, {}, true).size() == 2 &&
                 tracks(first, "Viewer").size() == 3 &&
                 !first.snapshot().frames.contains(initial[0].sid);
        },
        "Removed stream retained");
    const auto remaining = tracks(viewer, {}, true);
    const auto before = first.snapshot();
    until(
        [&] {
          const auto after = first.snapshot();
          for (const auto& track : remaining)
            if (!after.frames.contains(track.sid) ||
                after.frames.at(track.sid) == before.frames.at(track.sid))
              return false;
          return true;
        },
        "Other streams stalled after removal");
    for (const auto& track : remaining) {
      check(viewer.remove_video(track.sid), "Stop video rejected");
      until([&] { return !viewer.snapshot().changing_video; }, "Stop video timeout");
      check(viewer.snapshot().video_error.empty(), "Stop video failure");
    }
    until(
        [&] {
          const auto current = tracks(first, "Viewer");
          return current.size() == 1 && current[0].kind == demo::rtc::PublicationKind::audio;
        },
        "Video removal affected tone");
    check(viewer.add_video(), "Restart video rejected");
    until([&] { return !viewer.snapshot().changing_video && tracks(viewer, {}, true).size() == 1; },
          "Restart video timeout");
    const auto restarted = tracks(viewer, {}, true).front();
    check(restarted.name == "Test video 4", "Reused video name");
    until([&] { return first.snapshot().frames.contains(restarted.sid); },
          "Restarted video missing");
    check(viewer.add_video(), "Add before stop rejected");
    stopped(viewer);
    check(!viewer.snapshot().changing_video && !viewer.add_video() &&
              !viewer.remove_video(restarted.sid),
          "Operations survived disconnect");
    for (int i = 0; i < 3; ++i) {
      viewer.start(view);
      stopped(viewer);
    }
    auto wrong = options("WrongRoom");
    wrong.room = "wrong-room";
    viewer.start(wrong);
    until([&] { return viewer.snapshot().finished; }, "Room mismatch timeout");
    check(viewer.snapshot().state == demo::State::Error, "Room mismatch accepted");
    auto invalid = options("BadToken");
    invalid.connection.token = "invalid-token";
    viewer.start(invalid);
    until([&] { return viewer.snapshot().finished; }, "Bad token timeout");
    check(viewer.snapshot().state == demo::State::Error &&
              viewer.snapshot().error.find("invalid-token") == std::string::npos,
          "Token error/redaction");
    auto throwing = options("Throwing");
    throwing.media = {video};
    throwing.log_sink = [](const auto&) {
      throw std::runtime_error("Expected diagnostics failure");
    };
    viewer.start(throwing);
    until([&] { return viewer.snapshot().finished; }, "Observer exception cleanup");
    check(viewer.snapshot().state == demo::State::Error &&
              viewer.snapshot().error == "Expected diagnostics failure",
          "Observer failure propagation");
    view.media.clear();
    viewer.start(view);
    until([&] { return viewer.snapshot().state == demo::State::Connected; }, "Final connect");
    api.room().remove_participant(name, "Viewer");
    until([&] { return viewer.snapshot().finished; }, "Server removal not observed");
    check(viewer.snapshot().state == demo::State::Error && viewer.snapshot().frames.empty(),
          "Server removal state");
    stopped(first);
    std::cout << sdk::Json{{"ok", true},
                           {"separateTrackGraphs", true},
                           {"simultaneousVideos", 2},
                           {"videosPerParticipant", 3},
                           {"dynamicVideoPublishing", true},
                           {"recordings", true},
                           {"immediateCancellations", 3}}
                     .dump()
              << '\n';
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
