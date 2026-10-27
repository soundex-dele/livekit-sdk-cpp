#pragma once
#include "livekit/error.hpp"
#include <algorithm>
#include <chrono>
#include <future>
#include <stop_token>

namespace livekit::sdk::detail {
// WebRTC observers own their promises until completion. Abandoning this wait
// does not destroy an in-flight observer or run a deferred application task.
template<class T> T await_native(std::future<T>& result, std::stop_token stop,
    std::chrono::steady_clock::time_point deadline) {
  using Clock = std::chrono::steady_clock;
  for (;;) {
    if (stop.stop_requested())
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::cancelled, "Native operation cancelled");
    if (Clock::now() >= deadline)
      throw SDKError(ErrorDomain::transport, ErrorCode::timeout, "Native operation deadline exceeded", true);
    const auto state = result.wait_until(std::min(deadline, Clock::now() + std::chrono::milliseconds(10)));
    if (state == std::future_status::deferred)
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::unsupported, "Deferred task is not a native completion");
    if (state == std::future_status::ready) {
      // A completion racing cancellation/deadline must not start another SDP step.
      if (stop.stop_requested() || Clock::now() >= deadline) continue;
      return result.get();
    }
  }
}
}
