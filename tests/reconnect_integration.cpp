#include <livekit/livekit.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include "operations_integration.hpp"
#include "video_stream_integration.hpp"
#include "audio_stream_integration.hpp"
#include "video_source_integration.hpp"
#include "audio_source_integration.hpp"
#include "go_interop_integration.hpp"

namespace sdk = livekit::sdk;
using namespace std::chrono_literals;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> void until(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
    std::this_thread::sleep_for(20ms);
  }
}
int main(int argc, char** argv) {
  try {
    check(argc == 4 || argc == 5, "Usage: reconnect_integration server-url proxy-url marker-directory [rounds|--go-interop|--go-rpc-only|--operations|media-mode]");
    if (argc == 5 && std::string(argv[4]) == "--go-interop") return go_interop_test::run(argv[1], argv[3]);
    if (argc == 5 && std::string(argv[4]) == "--go-rpc-only") return go_interop_test::run_rpc_only(argv[1], argv[3]);
    if (argc == 5 && std::string(argv[4]) == "--operations") return operations_test::run(argv[1], argv[2], argv[3]);
    if (argc == 5 && std::string(argv[4]) == "--video-stream") return video_stream_test::run(argv[1], argv[2], argv[3]);
    if (argc == 5 && std::string(argv[4]) == "--audio-stream") return audio_stream_test::run(argv[1], argv[2], argv[3]);
    if (argc == 5 && std::string(argv[4]) == "--video-source") return video_source_test::run(argv[1], argv[2], argv[3]);
    if (argc == 5 && std::string(argv[4]) == "--video-source-rgba") return video_source_test::run(argv[1], argv[2], argv[3], true);
    if (argc == 5 && std::string(argv[4]) == "--audio-source") return audio_source_test::run(argv[1], argv[2], argv[3]);
    const int rounds = argc == 5 ? std::stoi(argv[4]) : 2;
    check(rounds >= 1 && rounds <= 1000, "Invalid recovery round count");
    const std::filesystem::path markers(argv[3]);
    auto token = [](const std::string& identity) {
      return sdk::make_token("reconnect-test", "reconnect-integration-secret-32-characters", identity,
          {{"roomJoin", true}, {"room", "reconnect"}, {"canPublish", true}, {"canSubscribe", true}, {"canPublishData", true}});
    };
    std::atomic<int> frames{0}, resumed{0}, reconnecting{0}, connected{0}, disconnected{0}, messages{0};
    std::atomic<bool> cancel_on_retry{false};
    sdk::Room* subscriber_ptr = nullptr;
    std::thread::id callback_thread;
    sdk::RoomCallback cb;
    cb.on_connected = [&] { callback_thread = std::this_thread::get_id(); ++connected; };
    cb.on_reconnecting = [&](const sdk::ReconnectInfo& info) {
      check(info.mode == sdk::ReconnectMode::resume, "Expected resume mode");
      check(std::this_thread::get_id() == callback_thread, "Reconnect callback thread changed");
      check(subscriber_ptr->connection_state() == sdk::ConnectionState::reconnecting, "Reconnect state not visible");
      ++reconnecting;
      if (cancel_on_retry) subscriber_ptr->request_stop();
    };
    cb.on_reconnected = [&](const sdk::ReconnectInfo&) {
      check(std::this_thread::get_id() == callback_thread, "Reconnected callback thread changed");
      check(subscriber_ptr->connection_state() == sdk::ConnectionState::connected, "Connected state not visible");
      ++resumed;
    };
    cb.on_disconnected = [&](int) { ++disconnected; };
    cb.on_data_received = [&](const auto& bytes, const auto&) { if (bytes == std::vector<uint8_t>{1, 2, 3}) ++messages; };
    sdk::Room subscriber(cb), publisher;
    subscriber_ptr = &subscriber;
    sdk::RoomOptions options;
    if (const auto protocol = std::getenv("LIVEKIT_TEST_PROTOCOL")) options.signal_protocol = std::stoi(protocol);
    options.video_frame_handler = [&](const sdk::VideoFrame& frame) {
      check(frame.width == 640 && frame.height == 360 && !frame.y.empty(), "Invalid decoded frame"); ++frames;
    };
    options.reconnect.initial_delay = 100ms;
    subscriber.join_with_token(argv[2], token("subscriber"), options);
    publisher.join_with_token(argv[1], token("publisher"));
    sdk::MediaSpec video; video.demo = true;
    auto outgoing = subscriber.local_participant().publish_track(video, {"resume-outgoing"}).get();
    auto publication = publisher.local_participant().publish_track(video, {"resume-video"}).get();
    until([&] { return frames.load() >= 15; }, "Initial video not received");
    const auto sid = subscriber.local_participant().sid();
    auto remote = subscriber.participant_by_identity("publisher");
    check(remote.has_value(), "Publisher missing");
    auto original_handle = remote->track_publications().at(0);
    for (int round = 1; round <= rounds; ++round) {
      std::ofstream(markers / ("cut-" + std::to_string(round))) << "cut";
      until([&] { return resumed.load() >= round || disconnected.load(); }, "Resume callback timeout");
      if (disconnected.load()) subscriber.wait();
      check(resumed.load() >= round, "Unexpected disconnect during resume");
      const int before = frames.load();
      until([&] { return frames.load() >= before + 15; }, "Video stopped after resume");
      check(subscriber.local_participant().sid() == sid, "Resume changed participant SID");
      check(original_handle.info().sid == publication.sid(), "Resume changed publication SID");
      original_handle.set_video_dimensions(640, 360).get();
      outgoing.set_muted(false).get(); // publisher transport/handle also survives ICE restart
      publisher.local_participant().publish_data({1, 2, 3}, {true, "resume"}).get();
      until([&] { return messages.load() >= round; }, "Data stopped after resume");
      check(connected == 1 && disconnected == 0, "Resume emitted initial connect/disconnect callbacks");
    }
    cancel_on_retry = true;
    const auto cancel_start = std::chrono::steady_clock::now();
    std::ofstream(markers / ("cut-" + std::to_string(rounds + 1))) << "cut";
    until([&] { return disconnected.load() == 1; }, "Cancellation during resume did not stop");
    subscriber.wait();
    check(std::chrono::steady_clock::now() - cancel_start < 3s, "Resume cancellation too slow");
    check(resumed == rounds, "Late reconnect callback after stop");
    publisher.disconnect(); publisher.wait();
    sdk::Room rejected;
    bool permission_error = false;
    try { rejected.join_with_token(argv[1], "invalid-token"); }
    catch (const sdk::SDKError& error) {
      permission_error = error.code() == sdk::ErrorCode::permission_denied && !error.retryable() &&
                         (error.backend_reason() == 401 || error.backend_reason() == 403);
    }
    check(permission_error, "Invalid token must fail without automatic retries");
    int token_requests = 0;
    sdk::RoomOptions refresh;
    refresh.token_provider = [&](sdk::TokenRequest request, sdk::TokenCompletion done) {
      check(request.reason == sdk::TokenRequestReason::authentication_failed, "Wrong refresh reason");
      check(request.url == "ws" + std::string(argv[1]).substr(4), "Wrong refresh endpoint");
      ++token_requests;
      done({token("refreshed")});
    };
    sdk::Room refreshed;
    refreshed.join_with_token(argv[1], "expired-token", refresh);
    check(token_requests == 1 && refreshed.local_participant().identity() == "refreshed", "HTTP 401 token refresh failed");
    refreshed.disconnect(); refreshed.wait();
    refresh.token_provider = [&](sdk::TokenRequest request, sdk::TokenCompletion done) {
      check(request.reason == sdk::TokenRequestReason::initial, "Missing initial token reason");
      ++token_requests; done({token("provider-initial")});
    };
    refreshed.join_with_token(argv[1], {}, refresh);
    check(token_requests == 2 && refreshed.local_participant().identity() == "provider-initial", "Initial provider failed");
    refreshed.disconnect(); refreshed.wait();
    refresh.token_provider = [&](sdk::TokenRequest, sdk::TokenCompletion done) {
      ++token_requests; done({"still-invalid-token"});
    };
    permission_error = false;
    try { refreshed.join_with_token(argv[1], "expired-token", refresh); }
    catch (const sdk::SDKError& error) { permission_error = error.code() == sdk::ErrorCode::permission_denied; }
    check(permission_error && token_requests == 3, "Token refresh must not retry indefinitely");
    std::cout << sdk::Json{{"ok", true}, {"resumes", resumed.load()}, {"frames", frames.load()},
                           {"messages", messages.load()}, {"cancelled", true}, {"tokenProvider", true}}.dump() << '\n';
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
