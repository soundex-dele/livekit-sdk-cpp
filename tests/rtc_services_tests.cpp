#include "rtc_services_contract.hpp"
#include "rpc_contract.hpp"
#include "backends/rtc/room_service_state.hpp"
#include <iostream>
namespace {
using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;
void service_lifecycle() {
  auto state = std::make_shared<livekit::sdk::rtc::detail::RoomServiceState>();
  auto service = state->handle();
  state->configure({}, 30ms); state->connect();
  auto send = service->send_data({1, 2, 3});
  const auto no_signal = [](const livekit::SignalRequest&) {};
  state->pump([](const auto&, auto) { return false; }, no_signal);
  rpc_contract_test::check(send.wait_for(0ms) != std::future_status::ready);
  std::this_thread::sleep_for(35ms);
  state->pump([](const auto&, auto) { return false; }, no_signal);
  bool timed_out = false;
  try { send.get(); } catch (const SDKError& error) { timed_out = error.code() == ErrorCode::timeout; }
  rpc_contract_test::check(timed_out);

  auto writer = service->open_stream(false);
  auto pending = writer->write("bounded stream");
  int packets = 0;
  state->pump([&](const auto&, auto) { ++packets; return true; }, no_signal, [] { return false; });
  rpc_contract_test::check(packets == 0 && pending.wait_for(0ms) != std::future_status::ready);
  state->pump([&](const auto&, auto) { ++packets; return true; }, no_signal, [] { return true; });
  pending.get(); rpc_contract_test::check(packets >= 2);
  auto interrupted = service->send_data({4}); state->interrupt();
  bool rejected = false;
  try { interrupted.get(); } catch (const SDKError&) { rejected = true; }
  rpc_contract_test::check(rejected);
  state->connect();
  auto next = service->send_data({5, 6});
  state->pump([&](const auto& label, auto bytes) { state->receive(label, bytes); return true; }, no_signal);
  next.get();
  auto event = service->next_event(0ms);
  rpc_contract_test::check(event && std::get<ReceivedData>(event->value).bytes == std::vector<uint8_t>({5, 6}));
  std::vector<std::future<void>> queued;
  for (int i = 0; i < 256; ++i) queued.push_back(service->send_data({7}));
  bool exhausted = false;
  try { service->send_data({8}).get(); } catch (const SDKError& error) { exhausted = error.code() == ErrorCode::resource_exhausted; }
  rpc_contract_test::check(exhausted);
  state->close(); state.reset();
  for (auto& future : queued) {
    rejected = false; try { future.get(); } catch (const SDKError&) { rejected = true; }
    rpc_contract_test::check(rejected);
  }
  rejected = false; try { service->send_data({9}).get(); } catch (const SDKError&) { rejected = true; }
  rpc_contract_test::check(rejected);
}
}
int main() {
  try {
    service_lifecycle();
    rtc_services_contract::streams();
    rtc_services_contract::encryption();
    rtc_services_contract::data_tracks(false);
    rtc_services_contract::data_tracks(true);
    rpc_contract();
    std::cout << "RTC service contracts passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
