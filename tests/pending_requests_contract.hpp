#pragma once
#include "engines/rtc/session.hpp"
#include "engines/rtc/pending_requests.hpp"

inline void pending_requests_contract() {
  using namespace livekit::sdk;
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  auto require = [](bool ok) { if (!ok) throw std::runtime_error("Pending request contract failed"); };
  auto error = [&](auto&& future, ErrorCode code) {
    require(future.wait_for(1s) == std::future_status::ready);
    try { future.get(); } catch (const SDKError& e) { require(e.code() == code); return; }
    throw std::runtime_error("Operation must fail");
  };
  auto state = std::make_shared<detail::RoomState>();
  state->running = true; state->generation = 1; state->connection = ConnectionState::connected;
  state->local.sid = "PA_local"; state->local.identity = "local";
  LocalParticipant local(state, 1, state->local, true);
  std::stop_source stopped; stopped.request_stop();
  OperationOptions cancelled; cancelled.stop = stopped.get_token();
  error(local.set_name("never-send", cancelled), ErrorCode::cancelled);
  require(state->take_commands().empty());
  OperationOptions timed; timed.timeout = 25ms;
  auto timeout = local.set_name("queued-timeout", timed);
  // No Room runner is dispatching commands: the timer must still complete it.
  error(timeout, ErrorCode::timeout);
  auto queued = state->take_commands(); require(queued.size() == 1 && queued.front()->done() && !queued.front()->begin_send());
  detail::PendingRequests requests;
  OperationOptions confirmed; confirmed.wait_for_ack = true;
  error(local.set_metadata("protocol12-unsupported", confirmed), ErrorCode::unsupported);
  require(state->take_commands().empty()); state->signal_protocol = 15;
  auto response = local.set_metadata("confirmed", confirmed);
  auto command = state->take_commands().front(); require(command->begin_send());
  auto id = requests.add(command);
  requests.respond(id, 4); require(response.wait_for(0ms) == std::future_status::timeout);
  requests.respond(id, 0); response.get(); requests.respond(id, 0); require(requests.size() == 0);
  auto denied = local.set_name("denied", confirmed);
  command = state->take_commands().front(); require(command->begin_send()); id = requests.add(command);
  requests.respond(id, 2); error(denied, ErrorCode::permission_denied);
  std::stop_source cancel;
  confirmed.stop = cancel.get_token();
  auto unknown = local.set_metadata("sent-then-cancelled", confirmed);
  command = state->take_commands().front(); require(command->begin_send()); id = requests.add(command);
  cancel.request_stop(); error(unknown, ErrorCode::outcome_unknown); requests.respond(id, 0); require(requests.size() == 0);
  confirmed.stop = {}; confirmed.timeout = 25ms;
  auto expired = local.set_metadata("sent-timeout", confirmed);
  command = state->take_commands().front(); require(command->begin_send()); id = requests.add(command);
  error(expired, ErrorCode::outcome_unknown); requests.respond(id, 0); require(requests.size() == 0);
  error(local.publish_data({1}, {}, confirmed), ErrorCode::unsupported);
  // Cancellation and acknowledgement arriving concurrently complete exactly once.
  for (int iteration = 0; iteration < 100; ++iteration) {
    auto race = std::make_shared<detail::Command>();
    std::atomic<int> completions{0};
    race->completion = [&](const Json&, std::exception_ptr) { ++completions; };
    require(race->begin_send());
    std::jthread first([&] { race->resolve(); });
    std::jthread second([&] { race->interrupt(ErrorCode::cancelled, "test cancellation"); });
    first.join(); second.join(); require(completions == 1);
  }
}
