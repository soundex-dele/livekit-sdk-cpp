#include "livekit/compat/thread.hpp"
#include "service_test_result.hpp"
#include "livekit/async_result.hpp"
#include "support/control_executor.hpp"
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

void abandoned_service_completion() {
  using rpc_contract_test::check;
  std::future<Result<int>> abandoned;
  {
    livekit::sdk::detail::ServicePromise<int> source;
    abandoned = source.get_future();
  }
  check(abandoned.wait_for(0ms) == std::future_status::ready);
  auto result = abandoned.get();
  check(!result && result.error().code == ErrorCode::cancelled);

  livekit::sdk::detail::ServicePromise<int> once;
  auto completed = once.get_future();
  once.set_value(17);
  once.set_failure(make_failure(ErrorDomain::data, ErrorCode::cancelled, "late cancellation"));
  check(service_value(completed.get()) == 17);

  livekit::sdk::detail::TaskExecutor executor(1, 4);
  std::promise<void> entered, release;
  auto running = entered.get_future(), released = release.get_future();
  check(executor.post([&] {
    entered.set_value();
    released.wait();
  }));
  check(running.wait_for(1s) == std::future_status::ready);
  auto pending = std::make_shared<livekit::sdk::detail::ServicePromise<int>>();
  auto discarded = pending->get_future();
  check(executor.post([pending] { pending->set_value(99); }));
  pending.reset();
  auto stopping = std::async(std::launch::async, [&] { executor.stop(); });
  check(discarded.wait_for(1s) == std::future_status::ready);
  result = discarded.get();
  check(!result && result.error().code == ErrorCode::cancelled);
  release.set_value();
  stopping.get();
}

void cancelled_control_reentry() {
  using rpc_contract_test::check;
  livekit::sdk::detail::ControlExecutor executor(4, [] {});
  std::promise<void> entered, release;
  auto running = entered.get_future(), released = release.get_future();
  service_value(executor.post([&] {
    entered.set_value();
    released.wait();
    return Result<void>::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::callback_failed,
                                              "Cancel pending controls"));
  }));
  check(running.wait_for(1s) == std::future_status::ready);
  auto source = std::make_shared<CompletionSource<int>>();
  auto observed = source->get_result().then(
      [](std::function<void()> task) {
        task();
        return Result<void>::success();
      },
      [&](AsyncResult<int> ready) {
        auto result = ready.get();
        auto reposted = executor.post([] { return Result<void>::success(); });
        check(!result && result.error().code == ErrorCode::cancelled && !reposted);
      });
  service_value(executor.post([source] {
    source->set_value(1);
    return Result<void>::success();
  }));
  source.reset();
  release.set_value();
  check(observed.wait_for(1s) == std::future_status::ready);
  service_value(observed.get());
  check(!executor.drain());
}

void indexed_send_deadlines() {
  using rpc_contract_test::check;
  auto state = std::make_shared<livekit::sdk::rtc::detail::RoomServiceState>();
  auto wake = std::make_shared<livekit::sdk::detail::WakeEvent>();
  service_value(state->attach_wakeup(wake));
  service_value(state->configure({}, 5min));
  service_value(state->connect());
  auto service = state->handle();
  const auto no_signal = [](const auto&) { return Result<void>::success(); };
  size_t send_attempts = 0;
  const auto blocked = [&](const auto&, auto) {
    ++send_attempts;
    return false;
  };
  auto head = service->send_data({1});
  livekit::compat::stop_source stop;
  DataSendOptions cancel_options;
  cancel_options.operation.stop = stop.get_token();
  auto cancelled = service->send_data({2}, cancel_options);
  DataSendOptions deadline_options;
  deadline_options.operation.timeout = 50ms;
  auto expired = service->send_data({3}, deadline_options);
  const auto deadline = state->next_data_deadline();
  check(deadline.has_value());
  const auto observed = wake->revision();
  service_value(state->pump(blocked, no_signal));
  check(send_attempts == 1 && wake->revision() == observed);
  stop.request_stop();
  check(wake->revision() != observed);
  service_value(state->pump(blocked, no_signal));
  check(cancelled.wait_for(0ms) == std::future_status::ready);
  {
    const auto result = cancelled.get();
    check(!result && result.error().code == ErrorCode::cancelled);
  }
  // Wait on the actual earliest deadline: the long-lived blocked head must not
  // hide cancellation or timeout of subsequent sends.
  wake->wait(wake->revision(), {}, deadline);
  service_value(state->pump(blocked, no_signal));
  check(expired.wait_for(0ms) == std::future_status::ready);
  {
    const auto result = expired.get();
    check(!result && result.error().code == ErrorCode::timeout);
  }
  check(head.wait_for(0ms) == std::future_status::timeout);
  check(service->snapshot().queued_sends == 1 && service->snapshot().scheduled_send_deadlines == 1);
  // A full send quantum explicitly schedules remaining work; a blocked sender
  // above did not continuously schedule itself.
  std::vector<std::future<Result<void>>> pending;
  for (int i = 0; i < 8; ++i) {
    pending.push_back(service->send_data({4}));
  }
  const auto before_send = wake->revision();
  service_value(state->pump([](const auto&, auto) { return true; }, no_signal));
  check(wake->revision() != before_send && service->snapshot().queued_sends == 5);
  service_value(head.get());
  service_value(state->pump([](const auto&, auto) { return true; }, no_signal));
  service_value(state->pump([](const auto&, auto) { return true; }, no_signal));
  for (auto& result : pending) {
    service_value(result.get());
  }
  check(!state->next_data_deadline() && service->snapshot().queued_sends == 0 &&
        service->snapshot().queued_send_bytes == 0);
  auto closing = service->send_data({5});
  state->interrupt();
  check(!state->next_data_deadline() && service->snapshot().scheduled_send_deadlines == 0);
  {
    const auto result = closing.get();
    check(!result && result.error().code == ErrorCode::connection_lost);
  }
  state->close();
}

void service_callbacks() {
  auto notifications = vi::TaskQueue::create("service.callback.contract");
  auto state = std::make_shared<livekit::sdk::rtc::detail::RoomServiceState>();
  std::promise<Failure> callback_error;
  auto error = callback_error.get_future();
  service_value(state->set_dispatcher(
      [&](auto task) {
        notifications->postTask(std::move(task));
        return Result<void>::success();
      },
      [&](auto failure) { callback_error.set_value(failure); }));
  auto service = state->handle();
  service_value(state->configure({}, 1s));
  service_value(state->connect());
  livekit::DataPacket packet;
  packet.mutable_user()->set_payload("callback");
  const auto bytes = packet.SerializeAsString();
  const auto receive = [&] {
    service_value(state->receive("_reliable",
                                 {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}));
  };
  std::promise<void> entered, release, delivered;
  auto entering = entered.get_future(), released = release.get_future(),
       done = delivered.get_future();
  const auto transport_thread = std::this_thread::get_id();
  service_value(service->set_event_handler([&](const DataEvent& event) {
    rpc_contract_test::check(std::this_thread::get_id() != transport_thread);
    rpc_contract_test::check(std::get<ReceivedData>(event.value).bytes.size() == 8);
    auto sent = service->send_data({1});
    entered.set_value();
    service_value(
        sent.get());  // Network/control progression must not require the notification queue.
    released.wait();
    delivered.set_value();
    return Result<void>::success();
  }));
  receive();
  rpc_contract_test::check(entering.wait_for(1s) == std::future_status::ready);
  service_value(state->pump([](const auto&, auto) { return true; },
                            [](const auto&) { return Result<void>::success(); }));
  auto clearing =
      std::async(std::launch::async, [&] { service_value(service->set_event_handler({})); });
  const bool fenced = clearing.wait_for(30ms) == std::future_status::timeout;
  release.set_value();
  clearing.get();
  rpc_contract_test::check(fenced && done.wait_for(1s) == std::future_status::ready);

  std::promise<void> self_removed;
  auto removed = self_removed.get_future();
  service_value(service->set_event_handler([&](const auto&) {
    service_value(service->set_event_handler({}));
    self_removed.set_value();
    return Result<void>::success();
  }));
  receive();
  rpc_contract_test::check(removed.wait_for(1s) == std::future_status::ready);
  rpc_contract_test::check(!service->snapshot().handler_registered);

  service_value(service->set_event_handler([](const auto&) {
    return Result<void>::failure(
        make_failure(ErrorDomain::data, ErrorCode::callback_failed, "data consumer failed"));
  }));
  receive();
  rpc_contract_test::check(error.wait_for(1s) == std::future_status::ready);
  auto info = error.get();
  const auto snapshot = service->snapshot();
  rpc_contract_test::check(snapshot.failure && snapshot.failure->id == info->id &&
                           info->code == ErrorCode::callback_failed &&
                           info->domain == ErrorDomain::data && !snapshot.handler_registered);
  state->close();
  service_value(service->set_event_handler({}));
}

void closed_service_callback_fence() {
  auto notifications = vi::TaskQueue::create("service.close.contract");
  auto state = std::make_shared<livekit::sdk::rtc::detail::RoomServiceState>();
  service_value(state->set_dispatcher(
      [&](auto task) {
        notifications->postTask(std::move(task));
        return Result<void>::success();
      },
      [](auto) {}));
  auto service = state->handle();
  service_value(state->configure({}, 1s));
  service_value(state->connect());
  std::promise<void> entered, release;
  auto entering = entered.get_future(), released = release.get_future();
  service_value(service->set_event_handler([&](const auto&) {
    entered.set_value();
    released.wait();
    return Result<void>::success();
  }));
  livekit::DataPacket packet;
  packet.mutable_user()->set_payload("hold");
  const auto bytes = packet.SerializeAsString();
  service_value(
      state->receive("_reliable", {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}));
  rpc_contract_test::check(entering.wait_for(1s) == std::future_status::ready);
  state->close();  // Backend shutdown must not wait for user callbacks.
  auto clearing =
      std::async(std::launch::async, [&] { service_value(service->set_event_handler({})); });
  const bool fenced = clearing.wait_for(30ms) == std::future_status::timeout;
  release.set_value();
  clearing.get();
  rpc_contract_test::check(fenced && service->snapshot().closed);
}

void inbound_packet_failures() {
  using rpc_contract_test::check;
  auto notifications = vi::TaskQueue::create("service.inbound.contract");
  auto state = std::make_shared<livekit::sdk::rtc::detail::RoomServiceState>();
  service_value(state->set_dispatcher(
      [&](auto task) {
        notifications->postTask(std::move(task));
        return Result<void>::success();
      },
      [](auto) {}));
  service_value(state->configure({}, 1s));
  service_value(state->connect());
  auto service = state->handle();
  app_support::DataEventMailbox events(service);
  const uint8_t invalid[] = {0xff};
  service_value(state->receive("_reliable", invalid));
  auto event = service_value(events.take_event(1s));
  check(event && std::holds_alternative<DataServiceError>(event->value));
  livekit::DataPacket bad_dtmf;
  bad_dtmf.mutable_sip_dtmf()->set_code(99);
  bad_dtmf.mutable_sip_dtmf()->set_digit("invalid");
  const auto wire = bad_dtmf.SerializeAsString();
  service_value(
      state->receive("_reliable", {reinterpret_cast<const uint8_t*>(wire.data()), wire.size()}));
  event = service_value(events.take_event(1s));
  check(event && std::holds_alternative<DataServiceError>(event->value));
  check(!service->snapshot().closed);
  state->close();

  auto rejected = std::make_shared<livekit::sdk::rtc::detail::RoomServiceState>();
  const auto cause = make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_argument,
                                  "Dispatcher rejected delivery");
  service_value(rejected->set_dispatcher([cause](auto) { return Result<void>::failure(cause); },
                                         [](auto) {}));
  service_value(rejected->configure({}, 1s));
  service_value(rejected->connect());
  service_value(rejected->set_event_handler([](const auto&) { return Result<void>::success(); }));
  auto received = rejected->receive("_reliable", invalid);
  check(!received && received.failure_record()->id == cause->id);
  rejected->close();
}

void service_lifecycle() {
  auto notifications = vi::TaskQueue::create("service.test.notifications");
  auto state = std::make_shared<livekit::sdk::rtc::detail::RoomServiceState>();
  service_value(state->set_dispatcher(
      [&](auto task) {
        notifications->postTask(std::move(task));
        return Result<void>::success();
      },
      [](auto) {}));
  auto service = state->handle();
  app_support::DataEventMailbox data_events(service);
  service_value(state->configure({}, 30ms));
  service_value(state->connect());
  auto send = service->send_data({1, 2, 3});
  const auto no_signal = [](const livekit::SignalRequest&) { return Result<void>::success(); };
  service_value(state->pump([](const auto&, auto) { return false; }, no_signal));
  rpc_contract_test::check(send.wait_for(0ms) != std::future_status::ready);
  std::this_thread::sleep_for(35ms);
  service_value(state->pump([](const auto&, auto) { return false; }, no_signal));
  const auto timed = send.get();
  const bool timed_out = !timed && timed.error().code == ErrorCode::timeout;
  rpc_contract_test::check(timed_out);

  auto writer = service_value(service->open_stream(false));
  auto pending = writer->write("bounded stream");
  int packets = 0;
  service_value(state->pump(
      [&](const auto&, auto) {
        ++packets;
        return true;
      },
      no_signal, [] { return false; }));
  rpc_contract_test::check(packets == 0 && pending.wait_for(0ms) != std::future_status::ready);
  service_value(state->pump(
      [&](const auto&, auto) {
        ++packets;
        return true;
      },
      no_signal, [] { return true; }));
  service_value(pending.get());
  rpc_contract_test::check(packets >= 2);
  auto interrupted = service->send_data({4});
  state->interrupt();
  bool rejected = !interrupted.get();
  rpc_contract_test::check(rejected);
  service_value(state->connect());
  auto next = service->send_data({5, 6});
  service_value(state->pump(
      [&](const auto& label, auto bytes) {
        service_value(state->receive(label, bytes));
        return true;
      },
      no_signal));
  service_value(next.get());
  auto event = service_value(data_events.take_event(1s));
  rpc_contract_test::check(event && std::get<ReceivedData>(event->value).bytes ==
                                        std::vector<uint8_t>({5, 6}));
  std::vector<std::future<Result<void>>> queued;
  for (int i = 0; i < 256; ++i) {
    queued.push_back(service->send_data({7}));
  }
  auto overflow = service->send_data({8}).get();
  const bool exhausted = !overflow && overflow.error().code == ErrorCode::resource_exhausted;
  rpc_contract_test::check(exhausted);
  state->close();
  state.reset();
  for (auto& future : queued) {
    rejected = !future.get();
    rpc_contract_test::check(rejected);
  }
  rejected = !service->send_data({9}).get();
  rpc_contract_test::check(rejected);
}
}  // namespace

int main() {
  abandoned_service_completion();
  cancelled_control_reentry();
  indexed_send_deadlines();
  service_lifecycle();
  inbound_packet_failures();
  service_callbacks();
  closed_service_callback_fence();
  rtc_services_contract::streams();
  rtc_services_contract::encryption();
  rtc_services_contract::data_tracks(false);
  rtc_services_contract::data_tracks(true);
  rpc_contract();
  std::cout << "RTC service contracts passed\n";
  return 0;
}
