#pragma once
#include "backends/rtc/livekit/token_request.hpp"
#include <future>
#include <thread>

inline void token_provider_contract() {
  using namespace livekit::sdk;
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  auto require = [](bool ok) {
    if (!ok)
      throw std::runtime_error("Token provider contract failed");
  };
  auto expect = [&](auto action, ErrorCode code, bool retryable) {
    try {
      action();
    } catch (const SDKError& e) {
      require(e.code() == code && e.retryable() == retryable);
      require(std::string(e.what()).find("secret-credential") == std::string::npos);
      return;
    }
    throw std::runtime_error("Token provider must fail");
  };
  const auto fetch = [&](const TokenProvider& provider, std::stop_token stop = {}) {
    return detail::request_token(provider, "https://rtc.example", TokenRequestReason::initial,
                                 Clock::now() + 1s, stop);
  };
  require(fetch([&](TokenRequest request, TokenCompletion complete) {
            require(request.url == "https://rtc.example" &&
                    request.reason == TokenRequestReason::initial);
            require(!request.stop.stop_requested() && request.deadline > Clock::now());
            complete({"first"});
            complete({"second"});
          }) == "first");
  std::jthread worker;
  require(fetch([&](TokenRequest, TokenCompletion complete) {
            worker = std::jthread([complete] { complete({"async"}); });
          }) == "async");
  worker.join();
  expect([&] { fetch([](auto, auto done) { done({{}, TokenFailure::unavailable}); }); },
         ErrorCode::connection_lost, true);
  expect([&] { fetch([](auto, auto done) { done({{}, TokenFailure::denied}); }); },
         ErrorCode::permission_denied, false);
  expect([&] { fetch([](auto, auto done) { done({}); }); }, ErrorCode::permission_denied, false);
  expect([&] { fetch([](auto, auto done) { done({std::string(65537, 'x')}); }); },
         ErrorCode::permission_denied, false);
  expect([&] { fetch([](auto, auto) { throw std::runtime_error("secret-credential"); }); },
         ErrorCode::permission_denied, false);
  TokenCompletion late;
  std::stop_token provider_stop;
  const auto started = Clock::now();
  expect(
      [&] {
        detail::request_token(
            [&](TokenRequest request, TokenCompletion done) {
              provider_stop = request.stop;
              late = std::move(done);
            },
            "https://rtc.example", TokenRequestReason::initial, Clock::now() + 30ms, {});
      },
      ErrorCode::timeout, true);
  require(provider_stop.stop_requested() && Clock::now() - started < 1s);
  late({"late-timeout"});  // safe after request and its result storage are destroyed
  std::stop_source cancel;
  std::promise<void> provider_started;
  auto pending = std::async(std::launch::async, [&] {
    expect(
        [&] {
          fetch(
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
  expect(
      [&] {
        fetch([](auto, auto) { throw std::runtime_error("must not start"); }, cancel.get_token());
      },
      ErrorCode::cancelled, false);
  expect(
      [&] {
        detail::request_token([](auto, auto) { throw std::runtime_error("must not start"); },
                              "https://rtc.example", TokenRequestReason::initial,
                              Clock::now() - 1ms, {});
      },
      ErrorCode::timeout, true);
}
