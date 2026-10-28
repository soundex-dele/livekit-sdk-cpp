#include <livekit/livekit.hpp>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include "media_transport_options.hpp"

namespace sdk = livekit::sdk;
using namespace std::chrono_literals;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> void until(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
    std::this_thread::sleep_for(20ms);
  }
}
int main(int argc, char** argv) {
  try {
    check(argc == 4, "Usage: rejoin_integration server-url proxy-url marker-directory");
    const std::filesystem::path markers(argv[3]);
    auto token = [](const std::string& identity) {
      return sdk::make_token("reconnect-test", "reconnect-integration-secret-32-characters", identity,
          {{"roomJoin", true}, {"room", "rejoin"}, {"canPublish", true}, {"canSubscribe", true},
           {"canPublishData", true}, {"canUpdateOwnMetadata", true}});
    };
    std::atomic<int> frames{0}, messages{0}, rejoins{0}, connected{0}, disconnected{0}, resumes{0};
    std::atomic<int> muted_frames{0};
    sdk::RoomCallback observe;
    observe.on_data_received = [&](const auto& data, const auto&) { if (data == std::vector<uint8_t>{4, 5}) ++messages; };
    sdk::Room observer(observe);
    sdk::RoomOptions watch;
    test_media_transport(watch);
    watch.video_frame_handler = [&](const sdk::VideoFrame&) { ++frames; };
    observer.join_with_token(argv[1], token("observer"), watch);
    sdk::MediaSpec demo; demo.demo = true;
    auto remote_video = observer.local_participant().publish_track(demo, {"remote-muted"}).get();
    sdk::RoomCallback cb;
    cb.on_connected = [&] { ++connected; };
    cb.on_reconnected = [&](const sdk::ReconnectInfo& info) {
      if (info.mode == sdk::ReconnectMode::rejoin) ++rejoins; else ++resumes;
    };
    cb.on_disconnected = [&](int) { ++disconnected; };
    sdk::Room publisher(cb);
    sdk::RoomOptions options;
    test_media_transport(options);
    options.reconnect.initial_delay = 100ms;
    options.regions.trusted_endpoints = {std::string(argv[2]) + "/alternate"};
    options.video_frame_handler = [&](const sdk::VideoFrame&) { ++muted_frames; };
    publisher.join_with_token(argv[2], token("publisher"), options);
    auto original_local = publisher.local_participant();
    auto video = original_local.publish_track(demo, {"camera", "stable-stream", sdk::TrackSource::camera}).get();
    sdk::MediaSpec tone; tone.video = false; tone.demo = true;
    auto audio = original_local.publish_track(tone, {"muted-audio"}).get();
    audio.set_muted(true).get();
    auto removed = original_local.publish_track(demo, {"removed"}).get();
    original_local.unpublish_track(removed.sid()).get();
    original_local.set_name("Restored name").get();
    original_local.set_attributes({{"recovery", "retained"}}).get();
    until([&] {
      auto info = publisher.local_participant().info();
      return info.name == "Restored name" && info.attributes.contains("recovery");
    }, "Initial metadata update missing");
    until([&] { return frames >= 15 && muted_frames >= 5; }, "Initial media missing");
    auto remote = publisher.participant_by_identity("observer");
    check(remote.has_value() && remote->track_publications().size() == 1, "Remote track missing");
    remote->track_publications().front().set_enabled(false).get();
    const auto stable_video = video.info().client_id;
    const auto stable_audio = audio.info().client_id;
    check(!stable_video.empty() && !stable_audio.empty() && stable_video != stable_audio, "Stable source IDs missing");
    std::string previous_sid = original_local.sid(), previous_video = video.sid();
    for (int round = 1; round <= 2; ++round) {
      std::ofstream(markers / ("rejoin-" + std::to_string(round))) << "rejoin";
      until([&] { return rejoins >= round || disconnected > 0; }, "Full rejoin timeout");
      if (disconnected) publisher.wait();
      check(rejoins >= round, "Unexpected terminal disconnect");
      const int before = frames.load();
      until([&] { return frames >= before + 15; }, "Published video did not resume");
      const auto local = publisher.local_participant();
      check(local.sid() != previous_sid, "Full rejoin reused participant SID");
      auto publications = local.track_publications();
      check(publications.size() == 2, "Missing, duplicated or resurrected publication");
      bool restored_video = false, restored_audio = false;
      for (const auto& p : publications) {
        const auto info = p.info();
        if (info.client_id == stable_video) {
          check(info.name == "camera" && info.source == sdk::TrackSource::camera && info.sid != previous_video,
                "Video publication options were not preserved");
          previous_video = info.sid; restored_video = true;
        }
        if (info.client_id == stable_audio) { check(info.muted, "Mute state not restored"); restored_audio = true; }
      }
      check(restored_video && restored_audio, "Stable source identity lost");
      previous_sid = local.sid();
      bool stale = false;
      try { video.set_muted(false).get(); } catch (const std::exception&) { stale = true; }
      check(stale, "Old publication handle can operate on new session");
      stale = false;
      try { original_local.publish_data({9}).get(); } catch (const std::exception&) { stale = true; }
      check(stale, "Old participant handle can operate on new session");
      local.publish_data({4, 5}, {true, "after-rejoin"}).get();
      until([&] { return messages >= round; }, "Reliable data failed after rejoin");
      until([&] {
        auto info = publisher.local_participant().info();
        return info.name == "Restored name" && info.attributes.contains("recovery") && info.attributes.at("recovery") == "retained";
      }, "Metadata not restored");
      // Allow already-decoded pre-disable frames to drain, then check settings
      // restoration against a continuously publishing peer.
      std::this_thread::sleep_for(300ms);
      const auto disabled_count = muted_frames.load();
      std::this_thread::sleep_for(500ms);
      check(muted_frames.load() == disabled_count, "Disabled remote stream restarted after rejoin");
      check(connected == 1 && disconnected == 0, "Rejoin emitted initial/terminal callbacks");
    }
    // Removal wins even if a malformed server also requests reconnect.
    std::ofstream(markers / "terminal") << "remove";
    until([&] { return disconnected == 1; }, "Terminal leave not handled");
    publisher.wait();
    check(publisher.disconnect_reason() == 4 && rejoins == 2 && resumes == 0, "Removed participant was resurrected");
    observer.disconnect(); observer.wait();
    std::cout << sdk::Json{{"ok", true}, {"rejoins", rejoins.load()}, {"frames", frames.load()},
                          {"messages", messages.load()}, {"stableSourceIds", true}, {"staleHandlesRejected", true},
                          {"removedTrackNotRepublished", true}, {"terminalLeave", true}}.dump() << '\n';
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
