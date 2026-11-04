#include "livekit/compat/algorithm.hpp"
#include "server_result.hpp"
#include "session.hpp"
#include <livekit/api.hpp>
#include <livekit/media_engine.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <atomic>

namespace sdk = livekit::sdk;
using namespace std::chrono_literals;

static void check(bool ok, const char* message) {
  if (!ok) {
    std::cerr << message << "\n";
    std::exit(1);
  }
}

static std::string env(const char* key) {
  auto* s = std::getenv(key);
  return s ? s : "";
}

template <class F>
static void until(F predicate, const char* error, int seconds = 20) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() > end) {
      std::cerr << error << "\n";
      std::exit(1);
    }
    std::this_thread::sleep_for(20ms);
  }
}

static std::vector<demo::rtc::PublicationInfo> tracks(const demo::Session& session,
                                                      const std::string& identity = {},
                                                      bool video_only = false) {
  const auto s = session.snapshot();
  if (s.state == demo::State::Error) {
    std::cerr << "Conference session: " + s.error << "\n";
    std::exit(1);
  }
  for (const auto& [sid, participant] : s.participants)
    if (identity.empty() ? sid == s.local_sid : participant.identity == identity) {
      auto result = participant.tracks;
      if (video_only)
        livekit::compat::erase_if(result, [](const auto& track) {
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
  {
    check(argc == 2, "Expected recording directory");
    const auto url = env("LIVEKIT_URL"), key = env("LIVEKIT_API_KEY"),
               secret = env("LIVEKIT_API_SECRET");
    sdk::LiveKitAPI api({url, {key, secret, ""}, false});
    const std::string name = "imgui-integration";
    std::filesystem::create_directories(std::filesystem::u8path(argv[1]));
    std::ofstream stats_log(std::filesystem::u8path(argv[1]) / "dashboard-stats.jsonl");
    std::atomic<uint64_t> receive_plis{0}, native_decoded{0}, receive_samples{0};
    server_result(api.room().create_room(name));

    struct Cleanup {
      sdk::LiveKitAPI& api;
      std::string name;

      ~Cleanup() {
        (void)api.room().delete_room(name);
      }
    } cleanup{api, name};

    auto options = [&](const std::string& identity) {
      demo::SessionOptions o;
      o.connection.url = url;
      o.room = name;
      if (identity == "Viewer")
        o.log_sink = [&](const sdk::Json& event) {
          stats_log << event.dump() << '\n';
          if (event.value("event", "") == "rtc-stats" && event.value("target", -1) == 1) {
            uint64_t plis = 0, decoded = 0;
            size_t videos = 0;
            for (const auto& row : event["stats"]) {
              if (row.value("type", "") == "inbound-rtp" && row.value("kind", "") == "video") {
                ++videos;
                plis += row.value("pliCount", uint64_t{0});
                decoded += row.value("framesDecoded", uint64_t{0});
              }
            }
            if (videos == 2) {
              receive_plis = plis;
              native_decoded = decoded;
              ++receive_samples;
            }
          }
          return sdk::Result<void>::success();
        };
      o.connection.token = server_result(api.room()
                                             .create_token()
                                             .set_identity(identity)
                                             .set_name(identity)
                                             .set_video_grant({{"roomJoin", true},
                                                               {"room", name},
                                                               {"canPublish", true},
                                                               {"canSubscribe", true}})
                                             .to_jwt());
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
    server_result(first.start(one));
    until([&] { return tracks(first, {}, true).size() == 1; }, "Initial publication");
    const auto first_video = tracks(first, {}, true).front().sid;
    auto view = options("Viewer");
    view.record = std::filesystem::u8path(argv[1]);
    view.connection.token = server_result(api.room()
                                              .create_token()
                                              .set_identity("Viewer")
                                              .set_name("Viewer")
                                              .set_video_grant({{"roomJoin", true},
                                                                {"room", name},
                                                                {"canPublish", false},
                                                                {"canPublishData", false},
                                                                {"canSubscribe", true}})
                                              .to_jwt());
    view.audio_playout = env("LIVEKIT_DEMO_TEST_AUDIO") == "1";
    server_result(viewer.start(view));
    until(
        [&] {
          const auto s = viewer.snapshot();
          return s.state == demo::State::Connected && s.frames.count(first_video);
        },
        "Initial participant/frame absent");
    check(demo::tiles(viewer.snapshot()).front().label == "Alice / Test video 1",
          "Initial typed participant mapping");
    auto two = options("Bob");
    two.media = {video};
    server_result(second.start(two));
    until([&] { return tracks(second, {}, true).size() == 1; }, "Second publication");
    const auto second_video = tracks(second, {}, true).front().sid;
    until(
        [&] {
          const auto s = viewer.snapshot();
          return s.participants.size() == 3 && s.frames.count(first_video) &&
                 s.frames.count(second_video) && s.received_frames > 120;
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
    until([&] { return receive_samples >= 3 && native_decoded >= 30; },
          "External decoder did not report real frames to WebRTC");
    const auto initial_plis = receive_plis.load();
    const auto initial_decoded = native_decoded.load();
    const auto initial_samples = receive_samples.load();
    until([&] { return receive_samples >= initial_samples + 4; },
          "Receiver statistics stopped during sustained decode");
    check(native_decoded > initial_decoded + 60, "Native decode callbacks must keep progressing");
    check(receive_plis == initial_plis, "Healthy external decode must not generate timeout PLIs");
    for (const auto& sid : {first_video, second_video}) {
      const auto snapshot = viewer.snapshot();
      const auto it = snapshot.video_stalls.find(sid);
      check(it != snapshot.video_stalls.end(), "Per-track stall tracker missing");
      const auto sample = it->second.sample();
      check(sample.active && !sample.waiting && sample.observed_seconds > 1 &&
                sample.rate_percent && *sample.rate_percent >= 0 && *sample.rate_percent <= 100,
            "Presentation frames must update per-track stall accounting");
    }
    std::cout << "PASS native_decode_feedback frames=" << native_decoded - initial_decoded
              << " timeout_pli_delta=" << receive_plis - initial_plis << '\n';
    server_result(api.room().mute_published_track(name, "Alice", first_video, true));
    until(
        [&] {
          for (const auto& tile : demo::tiles(viewer.snapshot()))
            if (tile.id == first_video && tile.status == "Video muted")
              return true;
          return false;
        },
        "Remote mute missing");
    check(!viewer.snapshot().video_stalls.at(first_video).sample().active,
          "Muted video must pause stall accounting");
    server_result(api.room().mute_published_track(name, "Alice", first_video, false));
    until(
        [&] {
          for (const auto& tile : demo::tiles(viewer.snapshot()))
            if (tile.id == first_video && tile.frame)
              return true;
          return false;
        },
        "Remote unmute missing");
    check(second.remove_video(second_video), "Remove second video rejected");
    until([&] { return !viewer.snapshot().frames.count(second_video); },
          "Unpublished frame retained");
    check(!viewer.snapshot().video_stalls.count(second_video), "Unpublished stall tracker retained");
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
    server_result(viewer.start(view));
    {
      until(
          [&] {
            return tracks(first, "Viewer").size() == 3 && tracks(viewer, {}, true).size() == 2;
          },
          "Multiple publications after reuse missing");
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
            if (!s.frames.count(track.sid))
              return false;
          return true;
        },
        "Dynamic streams did not all decode");
    check(viewer.remove_video(initial[0].sid), "Dynamic remove rejected");
    until(
        [&] {
          return !viewer.snapshot().changing_video && tracks(viewer, {}, true).size() == 2 &&
                 tracks(first, "Viewer").size() == 3 &&
                 !first.snapshot().frames.count(initial[0].sid);
        },
        "Removed stream retained");
    const auto remaining = tracks(viewer, {}, true);
    const auto before = first.snapshot();
    until(
        [&] {
          const auto after = first.snapshot();
          for (const auto& track : remaining)
            if (!after.frames.count(track.sid) ||
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
    until([&] { return first.snapshot().frames.count(restarted.sid); }, "Restarted video missing");
    check(viewer.add_video(), "Add before stop rejected");
    stopped(viewer);
    check(!viewer.snapshot().changing_video && !viewer.add_video() &&
              !viewer.remove_video(restarted.sid),
          "Operations survived disconnect");
    for (int i = 0; i < 3; ++i) {
      server_result(viewer.start(view));
      stopped(viewer);
    }
    auto wrong = options("WrongRoom");
    wrong.room = "wrong-room";
    server_result(viewer.start(wrong));
    until([&] { return viewer.snapshot().finished; }, "Room mismatch timeout");
    check(viewer.snapshot().state == demo::State::Error, "Room mismatch accepted");
    auto invalid = options("BadToken");
    invalid.connection.token = "invalid-token";
    server_result(viewer.start(invalid));
    until([&] { return viewer.snapshot().finished; }, "Bad token timeout");
    check(viewer.snapshot().state == demo::State::Error &&
              viewer.snapshot().error.find("invalid-token") == std::string::npos,
          "Token error/redaction");
    auto rejected = options("Rejected");
    rejected.media = {video};
    rejected.log_sink = [](const auto&) {
      return sdk::Result<void>::failure(sdk::make_failure(sdk::ErrorDomain::lifecycle,
                                                          sdk::ErrorCode::callback_failed,
                                                          "Expected diagnostics failure"));
    };
    server_result(viewer.start(rejected));
    until([&] { return viewer.snapshot().finished; }, "Observer result cleanup");
    check(viewer.snapshot().state == demo::State::Error &&
              viewer.snapshot().error == "Expected diagnostics failure",
          "Observer failure propagation");
    view.media.clear();
    server_result(viewer.start(view));
    until([&] { return viewer.snapshot().state == demo::State::Connected; }, "Final connect");
    server_result(api.room().remove_participant(name, "Viewer"));
    until([&] { return viewer.snapshot().finished; }, "Server removal not observed");
    check(viewer.snapshot().state == demo::State::Error && viewer.snapshot().frames.empty(),
          "Server removal state");
    stopped(first);
    // Exercise the same viewer with FFmpeg H265, not only the demo's VP8 source.
    sdk::client::MediaEngine hevc_publisher;
    auto hevc_options = options("H265 publisher");
    check(hevc_publisher.connect(hevc_options.connection), "H265 publisher connect");
    sdk::client::MediaPublishSpec hevc_spec;
    hevc_spec.name = "H265 bridge";
    hevc_spec.format = {sdk::client::Codec::h265, 90000, 0, 640, 360, {}};
    hevc_spec.input.generated = true;
    auto hevc_track = server_result(hevc_publisher.publish_media(hevc_spec));
    std::atomic<uint64_t> hevc_plis{0}, hevc_decoded{0}, hevc_samples{0};
    demo::Session hevc_viewer;
    auto hevc_view = options("H265 viewer");
    hevc_view.log_sink = [&](const sdk::Json& event) {
      stats_log << event.dump() << '\n';
      if (event.value("event", "") == "rtc-stats" && event.value("target", -1) == 1) {
        for (const auto& row : event["stats"]) {
          if (row.value("type", "") == "inbound-rtp" && row.value("kind", "") == "video") {
            hevc_plis = row.value("pliCount", uint64_t{0});
            hevc_decoded = row.value("framesDecoded", uint64_t{0});
            ++hevc_samples;
          }
        }
      }
      return sdk::Result<void>::success();
    };
    server_result(hevc_viewer.start(hevc_view));
    until([&] { return hevc_samples >= 3 && hevc_decoded >= 30; },
          "FFmpeg H265 decode feedback missing");
    const auto hevc_initial_plis = hevc_plis.load();
    const auto hevc_initial_frames = hevc_decoded.load();
    const auto hevc_initial_samples = hevc_samples.load();
    until([&] { return hevc_samples >= hevc_initial_samples + 4; }, "H265 stats stopped");
    check(hevc_decoded > hevc_initial_frames + 60, "H265 decoding did not continue");
    check(hevc_plis == hevc_initial_plis, "Healthy H265 decode generated timeout PLIs");
    check(!hevc_viewer.snapshot().frames.empty(), "H265 graph presentation output missing");
    std::cout << "PASS h265_decode_feedback frames=" << hevc_decoded - hevc_initial_frames
              << " timeout_pli_delta=" << hevc_plis - hevc_initial_plis << '\n';
    stopped(hevc_viewer);
    hevc_publisher.close();
    std::cout << sdk::Json{{"ok", true},
                           {"separateTrackGraphs", true},
                           {"simultaneousVideos", 2},
                           {"videosPerParticipant", 3},
                           {"dynamicVideoPublishing", true},
                           {"recordings", true},
                           {"immediateCancellations", 3}}
                     .dump()
              << '\n';
  }
}
