#pragma once
#include <livekit/livekit.hpp>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>

namespace video_stream_test {
using namespace livekit::sdk;
using namespace std::chrono_literals;
inline void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void until(F predicate, const char* message) {
  const auto end = std::chrono::steady_clock::now() + 15s;
  while (!predicate()) { check(std::chrono::steady_clock::now() < end, message); std::this_thread::sleep_for(10ms); }
}
inline int run(const char* server, const char* proxy, const char* directory) {
  auto token = [](const char* identity) {
    return make_token("reconnect-test", "reconnect-integration-secret-32-characters", identity,
      {{"roomJoin", true}, {"room", "video-stream"}, {"canPublish", true}, {"canSubscribe", true},
       {"canPublishData", true}, {"canUpdateOwnMetadata", true}});
  };
  auto stream = std::make_shared<VideoFrameStream>(VideoStreamOptions{2, 640 * 360 * 3});
  std::atomic<int> rejoins{0}, messages{0}, unpublished{0};
  RoomCallback callbacks;
  callbacks.on_data_received = [&](const auto& bytes, const auto&) { if (bytes == std::vector<uint8_t>{8, 9}) ++messages; };
  callbacks.on_reconnecting = [&](const ReconnectInfo&) {
    check(stream->stats().queued_frames == 0 && stream->stats().generation > 1, "Old media survived the rejoin boundary");
  };
  callbacks.on_reconnected = [&](const ReconnectInfo& info) { if (info.mode == ReconnectMode::rejoin) ++rejoins; };
  callbacks.on_track_unpublished = [&](const auto&, const auto&) { ++unpublished; };
  Room reader(callbacks), publisher;
  RoomOptions options; options.signal_protocol = 15; options.video_frame_stream = stream;
  reader.join_with_token(proxy, token("reader"), options);
  Room duplicate;
  bool refused = false;
  try { duplicate.join_with_token(server, token("duplicate"), options); }
  catch (const std::invalid_argument&) { refused = true; }
  check(refused && !stream->stats().closed, "Concurrent Room reused or closed another stream");
  publisher.join_with_token(server, token("publisher"));
  MediaSpec demo; demo.demo = true;
  auto publication = publisher.local_participant().publish_track(demo, {"video"}).get();
  // Deliberately do not read at all while signaling and data must stay live.
  until([&] { return stream->stats().received >= 20; }, "Pull output did not receive video");
  auto full = stream->stats();
  check(full.queued_frames == 2 && full.queued_bytes <= 640 * 360 * 3 && full.dropped >= 18, "Slow reader exceeded queue capacity");
  auto metadata = reader.local_participant().set_metadata("consumer-is-not-reading", OperationOptions{2s, {}, true});
  check(metadata.wait_for(3s) == std::future_status::ready, "Slow reader blocked confirmed control operation"); metadata.get();
  publisher.local_participant().publish_data({8, 9}, {true, "pull"}).get();
  until([&] { return messages == 1; }, "Slow reader blocked data events");
  auto retained = stream->read(1s);
  check(retained.status == VideoReadStatus::frame && retained.frame->width == 640 && retained.frame->height == 360 &&
      retained.frame->y.size() == 640 * 360 && retained.frame->stream_generation == 1, "Invalid owned I420 output");
  const auto first_generation = retained.frame->stream_generation;
  const auto first_pixels = retained.frame->y;
  std::ofstream(std::filesystem::path(directory) / "stream-rejoin") << "ready";
  until([&] { return rejoins == 1; }, "Pull output rejoin failed");
  auto recovered = stream->read(5s);
  check(recovered.status == VideoReadStatus::frame && recovered.frame->stream_generation > first_generation,
      "Rejoin output reused an old frame generation");
  const auto after = stream->stats().received;
  until([&] { return stream->stats().received >= after + 10; }, "Pull output did not resume real media");
  publisher.local_participant().unpublish_track(publication.sid()).get();
  until([&] { return unpublished > 0 && stream->stats().queued_frames == 0; }, "Unpublished track left stale pull frames");
  check(stream->read(50ms).status == VideoReadStatus::timeout, "Frame arrived after source removal");
  reader.disconnect(); reader.wait(); publisher.disconnect(); publisher.wait();
  check(stream->read(0ms).status == VideoReadStatus::closed && stream->stats().queued_bytes == 0, "Room shutdown did not close pull output");
  check(retained.frame->y == first_pixels, "Owned frame changed after rejoin/close");
  auto stats = stream->stats();
  std::cout << Json{{"ok", true}, {"received", stats.received}, {"dropped", stats.dropped}, {"rejoins", rejoins.load()},
    {"slowReaderDoesNotBlock", true}, {"singleRoomOwnership", true}, {"generationReset", true},
    {"unpublishPurgesFrames", true}, {"ownedFrameSurvivesClose", true}}.dump() << '\n';
  return 0;
}
}
