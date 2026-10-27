#include <livekit/livekit.hpp>
#include "api_contract.hpp"
#include "engines/rtc/events.hpp"
#include "engines/rtc/reconnect_policy.hpp"
#include "engines/rtc/recovery_state.hpp"
#include "token_provider_contract.hpp"
#include "region_policy_contract.hpp"
#include "room_moved_contract.hpp"
#include "engines/rtc/client_configuration.hpp"
#include "async_wait_contract.hpp"
#include "pending_requests_contract.hpp"
#include "audio_stream_contract.hpp"
#include "video_source_contract.hpp"
#include "audio_source_contract.hpp"
#include <iostream>
#include <sstream>
#include <thread>

namespace sdk = livekit::sdk;
static void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
  bool rejected = false;
  try { action(); } catch (const std::exception&) { rejected = true; }
  check(rejected, "Expected an exception");
}
int main(int argc, char** argv) {
  try {
    if (argc == 2) return api_contract(argv[1]);
    token_provider_contract();
    region_policy_contract();
    room_moved_contract();
    async_wait_contract();
    pending_requests_contract();
    audio_stream_contract();
    video_source_contract();
    audio_source_contract();
    livekit::ClientConfiguration client_config;
    check(!sdk::detail::relay_required(false, false, client_config), "Default relay policy");
    client_config.set_force_relay(livekit::ENABLED);
    check(sdk::detail::relay_required(false, false, client_config), "Server must force relay");
    rejects([&] { sdk::detail::relay_required(false, true, client_config); });
    client_config.set_force_relay(livekit::DISABLED);
    check(sdk::detail::relay_required(true, false, client_config), "Server cannot undo user relay policy");
    // No global event sink: two sessions route to different handlers, on the caller thread.
    int first = 0, second = 0;
    auto caller = std::this_thread::get_id();
    lk::EventEmitter a([&](const sdk::Json&) { check(std::this_thread::get_id() == caller, "Callback thread"); ++first; });
    lk::EventEmitter b([&](const sdk::Json&) { ++second; });
    std::thread producer([&] { a("a"); b("b"); }); producer.join();
    check(first == 0 && second == 0, "Producer must not call application code");
    a.dispatch(); a.rethrow(); check(first == 1 && second == 0, "Session isolation");
    b.dispatch(); b.rethrow(); check(second == 1, "Second event sink");
    lk::EventEmitter throwing([](const sdk::Json&) { throw std::runtime_error("callback failure"); });
    throwing("test"); throwing.dispatch(); rejects([&] { throwing.rethrow(); });

    sdk::Room room;
    sdk::RoomOptions options;
    check(sdk::version == "0.3.0" && sdk::protocol_version == 12, "Version/protocol baseline");
    sdk::ReconnectOptions reconnect;
    check(sdk::detail::reconnect_delay(reconnect, 1, 0).count() == 100, "Reconnect lower jitter bound");
    check(sdk::detail::reconnect_delay(reconnect, 2, 1).count() == 400, "Reconnect exponential backoff");
    check(sdk::detail::reconnect_delay(reconnect, 1000, 1).count() == 5000, "Reconnect delay cap");
    reconnect.max_attempts = 0;
    rejects([&] { sdk::detail::validate_reconnect(reconnect); });
    sdk::SDKError typed(sdk::ErrorDomain::signaling, sdk::ErrorCode::permission_denied, "denied", false, 403);
    check(!typed.retryable() && typed.backend_reason() == 403, "Typed failure semantics");
    sdk::ReconnectOptions budget_options; budget_options.max_attempts = 2;
    sdk::detail::RecoveryBudget budget(budget_options);
    const auto first_attempt = budget.next_attempt();
    const auto original_deadline = budget.deadline();
    check(first_attempt == 1 && budget.next_attempt() == 2 && budget.deadline() == original_deadline,
          "Resume/rejoin must share attempt and time budget");
    rejects([&] { budget.next_attempt(); });
    budget.reset(); check(budget.next_attempt() == 1, "Successful recovery resets attempt budget");
    check(sdk::detail::terminal_leave(2) && sdk::detail::terminal_leave(4) && sdk::detail::terminal_leave(5) &&
          !sdk::detail::terminal_leave(6), "Lifecycle removal must override reconnect requests");
    sdk::detail::RecoveryState sources;
    sdk::detail::DesiredPublication finite;
    finite.media.input = "finite.mp4";
    sources.publications["file"] = finite;
    rejects([&] { sources.validate_sources(); });
    sources.publications.at("file").media.reconnect = sdk::MediaRestartPolicy::restart;
    sources.validate_sources();
    sources.publications.at("file").media.reconnect = sdk::MediaRestartPolicy::fail;
    sources.publications.at("file").media.demo = true;
    rejects([&] { sources.validate_sources(); });
    rejects([&] { room.run(options); });
    options.url = "http://127.0.0.1:1"; options.token = "test"; options.room = "sdk";
    options.media.push_back({"", "ffmpeg", true, true, false, 641, 360, 30});
    rejects([&] { room.run(options); });
    options.media.clear();
    options.stats_interval_ms=100;
    rejects([&] { room.run(options); });
    options.stats_interval_ms=60001;
    rejects([&] { room.run(options); });
    options.stats_interval_ms=500;
    std::stop_source stopped; stopped.request_stop();
    // An already-cancelled valid session must not attempt a network connection.
    room.run(options, {}, stopped.get_token());
    room.run(options, {}, stopped.get_token());
    options.subscribe=false;options.video_frame_handler=[](const sdk::VideoFrame&){};
    rejects([&]{room.run(options,{},stopped.get_token());});
    sdk::MediaSpec preview;preview.demo=true;preview.local_preview=true;options.media={preview};
    room.run(options,{},stopped.get_token()); // publish-only local preview does not require subscription permission
    options.video_frame_stream = std::make_shared<sdk::VideoFrameStream>();
    rejects([&]{room.run(options,{},stopped.get_token());}); // ambiguous callback + pull output
    options.video_frame_handler = {};
    room.run(options,{},stopped.get_token());
    check(options.video_frame_stream->stats().closed, "Cancelled session did not close video reader");
    rejects([&]{room.run(options,{},stopped.get_token());}); // single-use stream
    options.video_frame_stream.reset();
    options.audio_frame_stream = std::make_shared<sdk::AudioFrameStream>();
    rejects([&]{room.run(options,{},stopped.get_token());}); // remote audio requires subscribe
    options.subscribe = true;
    room.run(options,{},stopped.get_token());
    check(options.audio_frame_stream->stats().closed, "Cancelled session left audio reader open");
    options.audio_frame_stream.reset();
    rejects([] { sdk::RoomServiceClient("http://localhost:7880", {}); });
    rejects([] { sdk::make_token("key", "secret", "id", {}, 0); });

    // Native smoke and public callback path; SDK must not write JSON to stdout.
    std::ostringstream output;
    auto* saved = std::cout.rdbuf(output.rdbuf());
    int smoke = 0;
    try {
      sdk::webrtc_smoke([&](const sdk::Json& event) {
        check(std::this_thread::get_id() == caller, "Native callback thread");
        if (event.at("event") == "webrtc-smoke") ++smoke;
      });
    } catch (...) { std::cout.rdbuf(saved); throw; }
    std::cout.rdbuf(saved);
    check(smoke == 1 && output.str().empty(), "SDK native smoke/output isolation");
    rejects([] { sdk::webrtc_smoke([](const sdk::Json&) { throw std::runtime_error("expected"); }); });
    sdk::webrtc_smoke(); // callback failure must leave the native runtime reusable
    std::cout << "SDK tests passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
