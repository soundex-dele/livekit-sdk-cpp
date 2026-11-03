#include "livekit/failure.hpp"
#include <cstdlib>
#include <livekit/livekit.hpp>
#include "api_contract.hpp"
#include "support/event_emitter.hpp"
#include "pcm_resampler_contract.hpp"
#include "video_source_contract.hpp"
#include "audio_source_contract.hpp"
#include <iostream>
#include <sstream>
#include <thread>

namespace sdk = livekit::sdk;

static void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << "\n";
    std::exit(1);
  }
}

int main(int argc, char** argv) {
  {
    if (argc == 2)
      return api_contract(argv[1]);
    video_source_contract();
    audio_source_contract();
    pcm_resampler_contract();
    // No global event sink: two sessions route to different handlers, on the caller thread.
    int first = 0, second = 0;
    auto caller = std::this_thread::get_id();
    lk::EventEmitter a([&](const sdk::Json&) {
      check(std::this_thread::get_id() == caller, "Callback thread");
      ++first;
    });
    lk::EventEmitter b([&](const sdk::Json&) { ++second; });
    std::thread producer([&] {
      a("a");
      b("b");
    });
    producer.join();
    check(first == 0 && second == 0, "Producer must not call application code");
    a.dispatch();
    check(bool(a.status()), "First event sink status");
    check(first == 1 && second == 0, "Session isolation");
    b.dispatch();
    check(bool(b.status()), "Second event sink status");
    check(second == 1, "Second event sink");
    lk::EventEmitter bounded([](const sdk::Json&) {});
    for (size_t i = 0; i != 4097; ++i) {
      bounded("test");
    }
    const auto overflow = bounded.status();
    check(!overflow && overflow.error().code == sdk::ErrorCode::resource_exhausted,
          "Event queue overflow must be explicit");

    check(sdk::version == "0.3.0" && sdk::protocol_version == 12, "Version/protocol baseline");
    auto typed = sdk::make_failure(sdk::ErrorDomain::signaling, sdk::ErrorCode::permission_denied,
                                   "denied", false, 403);
    check(!typed->retryable && typed->backend_reason == 403, "Typed failure semantics");
    check(!sdk::RoomServiceClient("http://localhost:7880", {}).status(),
          "Invalid service credentials");
    check(!sdk::make_token("key", "secret", "id", {}, 0), "Invalid token TTL");
    std::cout << "SDK tests passed\n";
    return 0;
  }
}
