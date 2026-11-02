#pragma once
#include "livekit/compat/thread.hpp"
#include <cstdlib>
#include "backends/rtc/livekit/token_request.hpp"
#include <future>
#include <thread>

inline void token_provider_contract() {
  using namespace livekit::sdk;
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  auto require = [](bool ok) {
    if (!ok)
      std::abort();
  };
  auto expect = [&](auto action, ErrorCode code, bool retryable) {
    auto result = action();
    require(!result && result.error().code == code && result.error().retryable == retryable);
    require(result.error().message.find("secret-credential") == std::string::npos);
  };
  auto accepted = [&](Result<std::string> result, const std::string& expected) {
    require(result && *result == expected);
  };
  const auto fetch = [&](const TokenProvider& provider, livekit::compat::stop_token stop = {}) {
    return detail::request_token(provider, "https://rtc.example", TokenRequestReason::initial,
                                 Clock::now() + 1s, stop);
  };
  accepted(fetch([&](TokenRequest request, TokenCompletion complete) {
             require(request.url == "https://rtc.example" &&
                     request.reason == TokenRequestReason::initial);
             require(!request.stop.stop_requested() && request.deadline > Clock::now());
             complete({"first"});
             complete({"second"});
           }),
           "first");
  livekit::compat::jthread worker;
  accepted(fetch([&](TokenRequest, TokenCompletion complete) {
             worker = livekit::compat::jthread([complete] { complete({"async"}); });
           }),
           "async");
  worker.join();
  expect([&] { return fetch([](auto, auto done) { done({{}, TokenFailure::unavailable}); }); },
         ErrorCode::connection_lost, true);
  expect([&] { return fetch([](auto, auto done) { done({{}, TokenFailure::denied}); }); },
         ErrorCode::permission_denied, false);
  expect([&] { return fetch([](auto, auto done) { done({}); }); }, ErrorCode::permission_denied,
         false);
  expect([&] { return fetch([](auto, auto done) { done({std::string(65537, 'x')}); }); },
         ErrorCode::permission_denied, false);
  TokenCompletion late;
  livekit::compat::stop_token provider_stop;
  const auto started = Clock::now();
  expect(
      [&] {
        return detail::request_token(
            [&](TokenRequest request, TokenCompletion done) {
              provider_stop = request.stop;
              late = std::move(done);
            },
            "https://rtc.example", TokenRequestReason::initial, Clock::now() + 30ms, {});
      },
      ErrorCode::timeout, true);
  require(provider_stop.stop_requested() && Clock::now() - started < 1s);
  late({"late-timeout"});  // safe after request and its result storage are destroyed
  livekit::compat::stop_source cancel;
  std::promise<void> provider_started;
  auto pending = std::async(std::launch::async, [&] {
    expect(
        [&] {
          return fetch(
              [&](TokenRequest request, TokenCompletion done) {
                provider_stop = request.stop;
                late = std::move(done);
                provider_started.set_value();
              },
              cancel.get_token());
        },
        ErrorCode::cancelled, false);
  });
  provider_started.get_future().get();
  cancel.request_stop();
  require(pending.wait_for(1s) == std::future_status::ready);
  pending.get();
  require(provider_stop.stop_requested());
  late({"late-cancel"});
  expect([&] { return fetch([](auto, auto) { std::abort(); }, cancel.get_token()); },
         ErrorCode::cancelled, false);
  expect(
      [&] {
        return detail::request_token([](auto, auto) { std::abort(); }, "https://rtc.example",
                                     TokenRequestReason::initial, Clock::now() - 1ms, {});
      },
      ErrorCode::timeout, true);
}
