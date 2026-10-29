#include "data_event_mailbox.hpp"
#include "utoolkit/task_queue/task_queue.h"
#include "rtc_services_contract.hpp"
#include "rpc_contract.hpp"
#include "backends/rtc/livekit/room_service_state.hpp"
#include <iostream>

namespace {
using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

void indexed_send_deadlines() {
  using rpc_contract_test::check;
  auto state = std::make_shared<livekit::sdk::rtc::detail::RoomServiceState>();
  auto wake = std::make_shared<livekit::sdk::detail::WakeEvent>();
  state->attach_wakeup(wake);
  state->configure({}, 5min);
  state->connect();
  auto service = state->handle();
  const auto no_signal = [](const auto&) {};
  size_t send_attempts = 0;
  const auto blocked = [&](const auto&, auto) {
    ++send_attempts;
    return false;
  };
  auto head = service->send_data({1});
  std::stop_source stop;
  DataSendOptions cancel_options;
  cancel_options.operation.stop = stop.get_token();
  auto cancelled = service->send_data({2}, cancel_options);
  DataSendOptions deadline_options;
  deadline_options.operation.timeout = 50ms;
  auto expired = service->send_data({3}, deadline_options);
  const auto deadline = state->next_data_deadline();
  check(deadline.has_value());
  const auto observed = wake->revision();
  state->pump(blocked, no_signal);
  check(send_attempts == 1 && wake->revision() == observed);
  stop.request_stop();
  check(wake->revision() != observed);
  state->pump(blocked, no_signal);
  check(cancelled.wait_for(0ms) == std::future_status::ready);
  try {
    cancelled.get();
    check(false);
  } catch (const SDKError& error) {
    check(error.code() == ErrorCode::cancelled);
  }
  // Wait on the actual earliest deadline: the long-lived blocked head must not
  // hide cancellation or timeout of subsequent sends.
  wake->wait(wake->revision(), {}, deadline);
  state->pump(blocked, no_signal);
  check(expired.wait_for(0ms) == std::future_status::ready);
  try {
    expired.get();
    check(false);
  } catch (const SDKError& error) {
    check(error.code() == ErrorCode::timeout);
  }
  check(head.wait_for(0ms) == std::future_status::timeout);
  check(service->snapshot().queued_sends == 1 && service->snapshot().scheduled_send_deadlines == 1);
  // A full send quantum explicitly schedules remaining work; a blocked sender
  // above did not continuously schedule itself.
  std::vector<std::future<void>> pending;
  for (int i = 0; i < 8; ++i) {
    pending.push_back(service->send_data({4}));
  }
  const auto before_send = wake->revision();
  state->pump([](const auto&, auto) { return true; }, no_signal);
  check(wake->revision() != before_send && service->snapshot().queued_sends == 5);
  head.get();
  state->pump([](const auto&, auto) { return true; }, no_signal);
  state->pump([](const auto&, auto) { return true; }, no_signal);
  for (auto& result : pending) {
    result.get();
  }
  check(!state->next_data_deadline() && service->snapshot().queued_sends == 0 &&
        service->snapshot().queued_send_bytes == 0);
  auto closing = service->send_data({5});
  state->interrupt();
  check(!state->next_data_deadline() && service->snapshot().scheduled_send_deadlines == 0);
  try {
    closing.get();
    check(false);
  } catch (const SDKError& error) {
    check(error.code() == ErrorCode::connection_lost);
  }
  state->close();
}

void service_callbacks() {
  auto notifications = vi::TaskQueue::create("service.callback.contract");
  auto state = std::make_shared<livekit::sdk::rtc::detail::RoomServiceState>();
  std::promise<std::exception_ptr> callback_error;
  auto error = callback_error.get_future();
  state->set_dispatcher([&](auto task) { notifications->postTask(std::move(task)); },
                        [&](auto failure) { callback_error.set_value(failure); });
  auto service = state->handle();
  state->configure({}, 1s);
  state->connect();
  livekit::DataPacket packet;
  packet.mutable_user()->set_payload("callback");
  const auto bytes = packet.SerializeAsString();
  const auto receive = [&] {
    state->receive("_reliable", {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
  };
  std::promise<void> entered, release, delivered;
  auto entering = entered.get_future(), released = release.get_future(),
       done = delivered.get_future();
  const auto transport_thread = std::this_thread::get_id();
  service->set_event_handler([&](const DataEvent& event) {
    rpc_contract_test::check(std::this_thread::get_id() != transport_thread);
    rpc_contract_test::check(std::get<ReceivedData>(event.value).bytes.size() == 8);
    auto sent = service->send_data({1});
    entered.set_value();
    sent.get();  // Network/control progression must not require the notification queue.
    released.wait();
    delivered.set_value();
  });
  receive();
  rpc_contract_test::check(entering.wait_for(1s) == std::future_status::ready);
  state->pump([](const auto&, auto) { return true; }, [](const auto&) {});
  auto clearing = std::async(std::launch::async, [&] { service->set_event_handler({}); });
  const bool fenced = clearing.wait_for(30ms) == std::future_status::timeout;
  release.set_value();
  clearing.get();
  rpc_contract_test::check(fenced && done.wait_for(1s) == std::future_status::ready);

  std::promise<void> self_removed;
  auto removed = self_removed.get_future();
  service->set_event_handler([&](const auto&) {
    service->set_event_handler({});
    self_removed.set_value();
  });
  receive();
  rpc_contract_test::check(removed.wait_for(1s) == std::future_status::ready);
  rpc_contract_test::check(!service->snapshot().handler_registered);

  service->set_event_handler([](const auto&) { throw std::runtime_error("data consumer failed"); });
  receive();
  rpc_contract_test::check(error.wait_for(1s) == std::future_status::ready);
  auto info = describe_failure(error.get());
  const auto snapshot = service->snapshot();
  rpc_contract_test::check(snapshot.failure && snapshot.failure->id == info->id &&
                           info->code == ErrorCode::callback_failed &&
                           info->domain == ErrorDomain::data && !snapshot.handler_registered);
  state->close();
  service->set_event_handler({});
}

void closed_service_callback_fence() {
  auto notifications = vi::TaskQueue::create("service.close.contract");
  auto state = std::make_shared<livekit::sdk::rtc::detail::RoomServiceState>();
  state->set_dispatcher([&](auto task) { notifications->postTask(std::move(task)); }, [](auto) {});
  auto service = state->handle();
  state->configure({}, 1s);
  state->connect();
  std::promise<void> entered, release;
  auto entering = entered.get_future(), released = release.get_future();
  service->set_event_handler([&](const auto&) {
    entered.set_value();
    released.wait();
  });
  livekit::DataPacket packet;
  packet.mutable_user()->set_payload("hold");
  const auto bytes = packet.SerializeAsString();
  state->receive("_reliable", {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
  rpc_contract_test::check(entering.wait_for(1s) == std::future_status::ready);
  state->close();  // Backend shutdown must not wait for user callbacks.
  auto clearing = std::async(std::launch::async, [&] { service->set_event_handler({}); });
  const bool fenced = clearing.wait_for(30ms) == std::future_status::timeout;
  release.set_value();
  clearing.get();
  rpc_contract_test::check(fenced && service->snapshot().closed);
}

void service_lifecycle() {
  auto notifications = vi::TaskQueue::create("service.test.notifications");
  auto state = std::make_shared<livekit::sdk::rtc::detail::RoomServiceState>();
  state->set_dispatcher([&](auto task) { notifications->postTask(std::move(task)); }, [](auto) {});
  auto service = state->handle();
  app_support::DataEventMailbox data_events(service);
  state->configure({}, 30ms);
  state->connect();
  auto send = service->send_data({1, 2, 3});
  const auto no_signal = [](const livekit::SignalRequest&) {};
  state->pump([](const auto&, auto) { return false; }, no_signal);
  rpc_contract_test::check(send.wait_for(0ms) != std::future_status::ready);
  std::this_thread::sleep_for(35ms);
  state->pump([](const auto&, auto) { return false; }, no_signal);
  bool timed_out = false;
  try {
    send.get();
  } catch (const SDKError& error) {
    timed_out = error.code() == ErrorCode::timeout;
  }
  rpc_contract_test::check(timed_out);

  auto writer = service->open_stream(false);
  auto pending = writer->write("bounded stream");
  int packets = 0;
  state->pump(
      [&](const auto&, auto) {
        ++packets;
        return true;
      },
      no_signal, [] { return false; });
  rpc_contract_test::check(packets == 0 && pending.wait_for(0ms) != std::future_status::ready);
  state->pump(
      [&](const auto&, auto) {
        ++packets;
        return true;
      },
      no_signal, [] { return true; });
  pending.get();
  rpc_contract_test::check(packets >= 2);
  auto interrupted = service->send_data({4});
  state->interrupt();
  bool rejected = false;
  try {
    interrupted.get();
  } catch (const SDKError&) {
    rejected = true;
  }
  rpc_contract_test::check(rejected);
  state->connect();
  auto next = service->send_data({5, 6});
  state->pump(
      [&](const auto& label, auto bytes) {
        state->receive(label, bytes);
        return true;
      },
      no_signal);
  next.get();
  auto event = data_events.take_event(1s);
  rpc_contract_test::check(event && std::get<ReceivedData>(event->value).bytes ==
                                        std::vector<uint8_t>({5, 6}));
  std::vector<std::future<void>> queued;
  for (int i = 0; i < 256; ++i)
    queued.push_back(service->send_data({7}));
  bool exhausted = false;
  try {
    service->send_data({8}).get();
  } catch (const SDKError& error) {
    exhausted = error.code() == ErrorCode::resource_exhausted;
  }
  rpc_contract_test::check(exhausted);
  state->close();
  state.reset();
  for (auto& future : queued) {
    rejected = false;
    try {
      future.get();
    } catch (const SDKError&) {
      rejected = true;
    }
    rpc_contract_test::check(rejected);
  }
  rejected = false;
  try {
    service->send_data({9}).get();
  } catch (const SDKError&) {
    rejected = true;
  }
  rpc_contract_test::check(rejected);
}
}  // namespace

int main() {
  try {
    indexed_send_deadlines();
    service_lifecycle();
    service_callbacks();
    closed_service_callback_fence();
    rtc_services_contract::streams();
    rtc_services_contract::encryption();
    rtc_services_contract::data_tracks(false);
    rtc_services_contract::data_tracks(true);
    rpc_contract();
    std::cout << "RTC service contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
