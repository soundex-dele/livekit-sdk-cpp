#include <livekit/livekit.hpp>
#include "api_contract.hpp"
#include "backends/rtc/events.hpp"
#include "backends/rtc/reconnect_policy.hpp"
#include "token_provider_contract.hpp"
#include "region_policy_contract.hpp"
#include "backends/rtc/client_configuration.hpp"
#include "video_source_contract.hpp"
#include "audio_source_contract.hpp"
#include "data_messages_contract.hpp"
#include "rpc_contract.hpp"
#include "subscribed_quality_contract.hpp"
#include <iostream>
#include <sstream>
#include <thread>

namespace sdk = livekit::sdk;

static void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

template <class F>
void rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "Expected an exception");
}

int main(int argc, char** argv) {
  try {
    if (argc == 2)
      return api_contract(argv[1]);
    token_provider_contract();
    region_policy_contract();
    video_source_contract();
    audio_source_contract();
    data_messages_contract();
    rpc_contract();
    subscribed_quality_contract();
    livekit::ClientConfiguration client_config;
    check(!sdk::detail::relay_required(false, false, client_config), "Default relay policy");
    client_config.set_force_relay(livekit::ENABLED);
    check(sdk::detail::relay_required(false, false, client_config), "Server must force relay");
    rejects([&] { sdk::detail::relay_required(false, true, client_config); });
    client_config.set_force_relay(livekit::DISABLED);
    check(sdk::detail::relay_required(true, false, client_config),
          "Server cannot undo user relay policy");
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
    a.rethrow();
    check(first == 1 && second == 0, "Session isolation");
    b.dispatch();
    b.rethrow();
    check(second == 1, "Second event sink");
    lk::EventEmitter throwing(
        [](const sdk::Json&) { throw std::runtime_error("callback failure"); });
    throwing("test");
    throwing.dispatch();
    rejects([&] { throwing.rethrow(); });

    check(sdk::version == "0.3.0" && sdk::protocol_version == 12, "Version/protocol baseline");
    sdk::ReconnectOptions reconnect;
    check(sdk::detail::reconnect_delay(reconnect, 1, 0).count() == 100,
          "Reconnect lower jitter bound");
    check(sdk::detail::reconnect_delay(reconnect, 2, 1).count() == 400,
          "Reconnect exponential backoff");
    check(sdk::detail::reconnect_delay(reconnect, 1000, 1).count() == 5000, "Reconnect delay cap");
    reconnect.max_attempts = 0;
    rejects([&] { sdk::detail::validate_reconnect(reconnect); });
    sdk::SDKError typed(sdk::ErrorDomain::signaling, sdk::ErrorCode::permission_denied, "denied",
                        false, 403);
    check(!typed.retryable() && typed.backend_reason() == 403, "Typed failure semantics");
    sdk::ReconnectOptions budget_options;
    budget_options.max_attempts = 2;
    sdk::detail::RecoveryBudget budget(budget_options);
    const auto first_attempt = budget.next_attempt();
    const auto original_deadline = budget.deadline();
    check(
        first_attempt == 1 && budget.next_attempt() == 2 && budget.deadline() == original_deadline,
        "Resume/rejoin must share attempt and time budget");
    rejects([&] { budget.next_attempt(); });
    budget.reset();
    check(budget.next_attempt() == 1, "Successful recovery resets attempt budget");
    check(sdk::detail::terminal_leave(2) && sdk::detail::terminal_leave(4) &&
              sdk::detail::terminal_leave(5) && !sdk::detail::terminal_leave(6),
          "Lifecycle removal must override reconnect requests");
    rejects([] { sdk::RoomServiceClient("http://localhost:7880", {}); });
    rejects([] { sdk::make_token("key", "secret", "id", {}, 0); });
    std::cout << "SDK tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
