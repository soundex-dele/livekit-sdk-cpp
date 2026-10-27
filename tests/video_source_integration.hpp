#pragma once
#include <livekit/livekit.hpp>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
#include <algorithm>

namespace video_source_test {
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
      {{"roomJoin", true}, {"room", "video-source"}, {"canPublish", true}, {"canSubscribe", true},
       {"canPublishData", true}, {"canUpdateOwnMetadata", true}});
  };
  auto source = std::make_shared<I420VideoSource>(I420SourceOptions{320, 180, 2, 320 * 180 * 3});
  auto stream = std::make_shared<VideoFrameStream>();
  auto preview = std::make_shared<VideoFrameStream>();
  VideoFrame pixels; pixels.width = 320; pixels.height = 180;
  pixels.stride_y = 324; pixels.stride_u = pixels.stride_v = 164;
  pixels.y.assign(324 * 180, 76); pixels.u.assign(164 * 90, 94); pixels.v.assign(164 * 90, 180);
  std::atomic<int> rejoined{0}, written{0}, rejected{0};
  RoomCallback cb; cb.on_reconnected = [&](const ReconnectInfo& info) { if (info.mode == ReconnectMode::rejoin) ++rejoined; };
  Room publisher(cb), reader;
  RoomOptions po; po.signal_protocol = 15; po.video_frame_stream = preview;
  publisher.join_with_token(proxy, token("writer"), po);
  RoomOptions ro; ro.video_frame_stream = stream;
  reader.join_with_token(server, token("reader"), ro);
  MediaSpec spec; spec.width = 320; spec.height = 180; spec.video_source = source; spec.local_preview = true;
  auto publication = publisher.local_participant().publish_track(spec, {"custom-i420"}).get();
  const auto stable_id = publication.info().client_id;
  std::jthread writer([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      const auto status = source->write(pixels);
      if (status == VideoWriteStatus::accepted) ++written;
      else if (status == VideoWriteStatus::backpressure) ++rejected;
      else break;
      std::this_thread::sleep_for(2ms);
    }
  });
  auto verify_frame = [&] {
    const auto local = preview->read(5s);
    check(local.status == VideoReadStatus::frame && local.frame->width == 320 && local.frame->height == 180 &&
        std::all_of(local.frame->y.begin(), local.frame->y.end(), [](auto p) { return p == 76; }) &&
        std::all_of(local.frame->u.begin(), local.frame->u.end(), [](auto p) { return p == 94; }) &&
        std::all_of(local.frame->v.begin(), local.frame->v.end(), [](auto p) { return p == 180; }), "I420 pixels changed before encoding");
    const auto result = stream->read(5s);
    check(result.status == VideoReadStatus::frame, "Custom source produced no remote video");
    const auto& f = *result.frame;
    std::cerr << Json{{"width", f.width}, {"height", f.height}, {"y", f.y.at(1000)},
        {"u", f.u.at(500)}, {"v", f.v.at(500)}}.dump() << '\n';
    // VP8 is lossy. Check every decoded pixel with an explicit 8/255 bound;
    // the local graph check above separately requires exact input copies.
    check(f.width == 320 && f.height == 180 &&
        std::all_of(f.y.begin(), f.y.end(), [](auto p) { return std::abs(int(p) - 76) <= 8; }) &&
        std::all_of(f.u.begin(), f.u.end(), [](auto p) { return std::abs(int(p) - 94) <= 8; }) &&
        std::all_of(f.v.begin(), f.v.end(), [](auto p) { return std::abs(int(p) - 180) <= 8; }), "Custom I420 decoded pixels exceeded VP8 tolerance");
  };
  until([&] { return stream->stats().received >= 15; }, "Initial I420 input not received");
  verify_frame();
  bool refused = false;
  try { publisher.local_participant().publish_track(spec).get(); } catch (const std::exception&) { refused = true; }
  check(refused && source->stats().accepting, "Duplicate publication stole I420 source");
  refused = false;
  try { reader.local_participant().publish_track(spec).get(); } catch (const std::exception&) { refused = true; }
  check(refused && source->stats().accepting, "Another Room stole I420 source");
  publisher.local_participant().set_metadata("producer-under-load", OperationOptions{2s, {}, true}).get();
  check(source->stats().queued_frames <= 2 && source->stats().queued_bytes <= 320 * 180 * 3 && rejected > 0, "Input backpressure failed");
  const auto generation = source->stats().generation;
  std::ofstream(std::filesystem::path(directory) / "source-rejoin") << "ready";
  until([&] { return rejoined == 1 && source->stats().generation > generation; }, "I420 source did not restart on rejoin");
  const auto received = stream->stats().received;
  until([&] { return stream->stats().received >= received + 15; }, "I420 source stalled after rejoin");
  verify_frame();
  auto tracks = publisher.local_participant().track_publications();
  check(tracks.size() == 1 && tracks.front().info().client_id == stable_id, "Recovered source duplicated or changed logical ID");
  bool stale = false; try { publication.set_muted(false).get(); } catch (const std::exception&) { stale = true; }
  check(stale, "Old custom source handle survived rejoin");
  publisher.local_participant().unpublish_track(tracks.front().sid()).get();
  until([&] { return !source->stats().attached; }, "Unpublish did not release I420 consumer");
  check(source->stats().queued_frames == 0 && source->write(pixels) == VideoWriteStatus::backpressure, "Inactive source retained frames");
  publication = publisher.local_participant().publish_track(spec).get();
  until([&] { return source->stats().accepting; }, "I420 source could not be republished");
  const auto again = stream->stats().received;
  until([&] { return stream->stats().received >= again + 5; }, "Republished source produced no video");
  source->close(); writer.request_stop(); writer.join();
  check(source->write(pixels) == VideoWriteStatus::closed && source->stats().queued_bytes == 0, "EOS not permanent");
  publisher.local_participant().unpublish_track(publication.sid()).get();
  // A source waiting for its very first application frame must also stop.
  spec.video_source = std::make_shared<I420VideoSource>(I420SourceOptions{320,180});
  publisher.local_participant().publish_track(spec).get();
  until([&] { return spec.video_source->stats().accepting; }, "Empty source never activated");
  const auto before = std::chrono::steady_clock::now();
  publisher.disconnect(); publisher.wait(); reader.disconnect(); reader.wait();
  check(std::chrono::steady_clock::now() - before < 3s && !spec.video_source->stats().attached, "Shutdown blocked on empty I420 source");
  std::cout << Json{{"ok", true}, {"received", stream->stats().received}, {"accepted", written.load()},
    {"backpressure", rejected.load()}, {"rejoins", rejoined.load()}, {"pixelCheck", true},
    {"exactLocalPixels", true}, {"decodedPixelTolerance", 8},
    {"exclusiveConsumer", true}, {"stableSourceId", true}, {"republish", true}, {"emptySourceShutdown", true}}.dump() << '\n';
  return 0;
}
}
