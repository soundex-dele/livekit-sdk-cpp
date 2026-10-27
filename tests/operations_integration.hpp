#pragma once
#include <livekit/livekit.hpp>
#include <atomic>
#include <fstream>
#include <thread>

namespace operations_test {
using namespace livekit::sdk;
using namespace std::chrono_literals;
inline void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void until(F predicate, const char* message) {
  const auto end = std::chrono::steady_clock::now() + 15s;
  while (!predicate()) { check(std::chrono::steady_clock::now() < end, message); std::this_thread::sleep_for(10ms); }
}
template<class T> void expect(std::future<T>& future, ErrorCode code) {
  check(future.wait_for(3s) == std::future_status::ready, "Operation future did not complete");
  try { future.get(); } catch (const SDKError& error) { check(error.code() == code, "Wrong operation error"); return; }
  throw std::runtime_error("Operation should have failed");
}
inline int run(const char* server, const char* proxy, const char* directory) {
  const std::filesystem::path markers(directory);
  auto marker = [&](const char* name) { std::ofstream(markers / name) << "ready"; };
  auto wait_marker = [&](const char* name) { until([&] { return std::filesystem::exists(markers / name); }, "Proxy marker timeout"); };
  auto token = [](const char* identity, bool metadata_allowed = true) {
    return make_token("reconnect-test", "reconnect-integration-secret-32-characters", identity,
      {{"roomJoin", true}, {"room", "reconnect"}, {"canPublish", true}, {"canSubscribe", true}, {"canPublishData", true}, {"canUpdateOwnMetadata", metadata_allowed}});
  };
  std::atomic<int> frames{0}, rejoined{0}, forbidden_publications{0};
  RoomOptions observer_options;
  observer_options.video_frame_handler = [&](const VideoFrame&) { ++frames; };
  Room observer; observer.join_with_token(server, token("observer"), observer_options);
  RoomCallback callback;
  callback.on_reconnected = [&](const ReconnectInfo& info) { if (info.mode == ReconnectMode::rejoin) ++rejoined; };
  callback.on_local_track_published = [&](const TrackPublication& p, const LocalParticipant&) {
    if (p.info().name == "cancelled-publication") ++forbidden_publications;
  };
  RoomOptions operation_options; operation_options.signal_protocol = 15;
  Room room(callback); room.join_with_token(proxy, token("operations"), operation_options);
  OperationOptions confirmed; confirmed.wait_for_ack = true; confirmed.timeout = 2s;
  auto unsupported = observer.local_participant().set_metadata("protocol12", confirmed);
  expect(unsupported, ErrorCode::unsupported);
  Room denied_room; denied_room.join_with_token(server, token("denied", false), operation_options);
  auto real_denied = denied_room.local_participant().set_metadata("not-allowed", confirmed);
  expect(real_denied, ErrorCode::permission_denied); denied_room.disconnect(); denied_room.wait();
  room.local_participant().set_metadata("ack-ok", confirmed).get();
  std::stop_source cancel;
  confirmed.stop = cancel.get_token();
  auto cancelled = room.local_participant().set_metadata("hold-cancel", confirmed);
  wait_marker("held-cancel");
  check(cancelled.wait_for(20ms) == std::future_status::timeout, "Operation resolved before server acknowledgement");
  cancel.request_stop(); expect(cancelled, ErrorCode::outcome_unknown); marker("release-cancel");
  confirmed.stop = {};
  room.local_participant().set_metadata("ack-next", confirmed).get();
  auto queued = room.local_participant().set_metadata("hold-queued", confirmed);
  wait_marker("held-queued");
  check(queued.wait_for(30ms) == std::future_status::timeout, "QUEUED response was mistaken for final confirmation");
  marker("release-queued"); queued.get();
  confirmed.timeout = 250ms;
  auto timed = room.local_participant().set_metadata("drop-ack", confirmed);
  expect(timed, ErrorCode::outcome_unknown);
  confirmed.timeout = 2s;
  auto denied = room.local_participant().set_metadata("inject-denied", confirmed);
  expect(denied, ErrorCode::permission_denied);
  std::stop_source already_cancelled; already_cancelled.request_stop();
  confirmed.stop = already_cancelled.get_token();
  auto unsent = room.local_participant().set_metadata("must-not-send", confirmed);
  expect(unsent, ErrorCode::cancelled);
  MediaSpec video; video.demo = true;
  auto base = room.local_participant().publish_track(video, {"retained"}).get();
  until([&] { return frames >= 10; }, "Initial operation test media missing");
  std::stop_source cancel_publish;
  OperationOptions publishing; publishing.stop = cancel_publish.get_token(); publishing.timeout = 5s;
  auto cancelled_track = room.local_participant().publish_track(video, {"cancelled-publication"}, publishing);
  wait_marker("held-publication"); cancel_publish.request_stop();
  expect(cancelled_track, ErrorCode::outcome_unknown); marker("release-publication");
  wait_marker("released-publication");
  room.local_participant().set_metadata("after-cancel", OperationOptions{2s, {}, true}).get();
  check(forbidden_publications == 0 && room.local_participant().track_publications().size() == 1,
      "Cancelled publication was retained");
  marker("operations-rejoin");
  until([&] { return rejoined == 1; }, "Operation cleanup rejoin failed");
  until([&] { return room.local_participant().track_publications().size() == 1; }, "Confirmed publication not restored");
  const auto tracks = room.local_participant().track_publications();
  check(tracks.front().info().client_id == base.info().client_id && tracks.front().info().name == "retained",
      "Cancelled source replayed or retained source changed");
  const int before = frames.load();
  until([&] { return frames >= before + 10; }, "Media missing after operation cleanup/rejoin");
  publishing.stop = {}; publishing.timeout = 250ms;
  auto lost_publication = room.local_participant().publish_track(video, {"drop-publication"}, publishing);
  wait_marker("dropped-publication"); expect(lost_publication, ErrorCode::outcome_unknown);
  until([&] { return rejoined == 2; }, "Missing publication ACK did not reconcile transport");
  const auto final_tracks = room.local_participant().track_publications();
  check(final_tracks.size() == 1 && final_tracks.front().info().client_id == base.info().client_id,
      "Unconfirmed publication was replayed after timeout");
  room.local_participant().set_metadata("after-timeout", OperationOptions{2s, {}, true}).get();
  const int final_before = frames.load();
  until([&] { return frames >= final_before + 10; }, "Media missing after publication timeout");
  room.disconnect(); room.wait(); observer.disconnect(); observer.wait();
  std::cout << Json{{"ok", true}, {"confirmedMetadata", true}, {"cancelled", true}, {"deadline", true},
    {"queuedNotFinal", true}, {"lateAckIgnored", true}, {"cancelledPublicationNotReplayed", true},
    {"expiredPublicationNotReplayed", true}, {"realPermissionDenial", true}, {"rejoins", rejoined.load()}, {"frames", frames.load()}}.dump() << '\n';
  return 0;
}
}
