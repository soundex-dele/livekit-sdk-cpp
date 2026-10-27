#pragma once
#include "engines/rtc/async_wait.hpp"
#include <thread>
#include <source_location>

inline void async_wait_contract() {
  using namespace livekit::sdk;
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  auto require = [](bool ok, std::source_location where = std::source_location::current()) {
    if (!ok) throw std::runtime_error("Native wait contract failed at line " + std::to_string(where.line()));
  };
  auto bounded = [&](Clock::time_point start, const char* operation) {
    const auto elapsed = Clock::now() - start;
    if (elapsed >= 500ms) throw std::runtime_error(std::string("Native wait ") + operation + " took " +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) + " ms (limit 500 ms)");
  };
  auto expect = [&](auto&& fn, ErrorCode code) {
    try { fn(); } catch (const SDKError& e) { require(e.code() == code); return; }
    throw std::runtime_error("Native wait must fail");
  };
  std::promise<int> completed;
  auto result = completed.get_future(); completed.set_value(42);
  require(detail::await_native(result, {}, Clock::now() + 1s) == 42);
  std::promise<void> late; auto timed = late.get_future();
  auto start = Clock::now();
  expect([&] { detail::await_native(timed, {}, start + 25ms); }, ErrorCode::timeout);
  bounded(start, "timeout");
  late.set_value(); // Observer completion after timeout remains safe.
  std::promise<void> pending; auto cancel_result = pending.get_future();
  std::stop_source cancel;
  std::jthread canceller([&] { std::this_thread::sleep_for(20ms); cancel.request_stop(); });
  start = Clock::now();
  expect([&] { detail::await_native(cancel_result, cancel.get_token(), start + 30s); }, ErrorCode::cancelled);
  bounded(start, "cancellation"); pending.set_value();
  std::promise<void> expired; auto expired_result = expired.get_future(); expired.set_value();
  expect([&] { detail::await_native(expired_result, {}, Clock::now() - 1ms); }, ErrorCode::timeout);
  auto deferred = std::async(std::launch::deferred, [] { throw std::runtime_error("must not run"); });
  expect([&] { detail::await_native(deferred, {}, Clock::now() + 1s); }, ErrorCode::unsupported);
}
