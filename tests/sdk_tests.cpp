#include <livekit/livekit.hpp>
#include "api_contract.hpp"
#include "lk/events.hpp"
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
    rejects([&] { room.run(options); });
    options.url = "http://127.0.0.1:1"; options.token = "test"; options.room = "sdk";
    options.media.push_back({"", "ffmpeg", true, true, false, 641, 360, 30});
    rejects([&] { room.run(options); });
    options.media.clear();
    std::stop_source stopped; stopped.request_stop();
    // An already-cancelled valid session must not attempt a network connection.
    room.run(options, {}, stopped.get_token());
    room.run(options, {}, stopped.get_token());
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
