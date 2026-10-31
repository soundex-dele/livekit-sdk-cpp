#pragma once
#include "livekit/token_provider.hpp"
#include "livekit/result.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

namespace livekit::sdk::detail {
inline Result<std::string> request_token(const TokenProvider& provider, std::string url,
                                         TokenRequestReason reason,
                                         std::chrono::steady_clock::time_point deadline,
                                         std::stop_token stop) {
  using Clock = std::chrono::steady_clock;

  struct Pending {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::optional<TokenResult> result;
    bool closed = false;
  };

  const auto check = [&]() -> Failure {
    if (stop.stop_requested())
      return make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled, "Token request cancelled");
    if (Clock::now() >= deadline)
      return make_failure(ErrorDomain::signaling, ErrorCode::timeout,
                          "Token request deadline exceeded", true);
    return {};
  };
  if (auto failure = check()) {
    return Result<std::string>::failure(std::move(failure));
  }
  auto pending = std::make_shared<Pending>();
  std::stop_source cancellation;
  std::stop_callback propagate(stop, [&] { cancellation.request_stop(); });
  TokenCompletion complete = [weak = std::weak_ptr(pending)](TokenResult result) {
    if (auto state = weak.lock()) {
      std::lock_guard lock(state->mutex);
      if (state->closed || state->result)
        return;
      // Bound data retained even if an application provider misbehaves.
      if (result.token.size() > 65536)
        result = {{}, TokenFailure::denied};
      state->result.emplace(std::move(result));
      state->changed.notify_all();
    }
  };
  provider({std::move(url), reason, deadline, cancellation.get_token()}, std::move(complete));
  std::unique_lock lock(pending->mutex);
  pending->changed.wait_until(lock, stop, deadline, [&] { return pending->result.has_value(); });
  pending->closed = true;
  auto result = std::move(pending->result);
  lock.unlock();
  cancellation.request_stop();
  if (auto failure = check()) {
    return Result<std::string>::failure(std::move(failure));
  }
  if (!result || result->failure != TokenFailure::none || result->token.empty()) {
    const bool retryable = result && result->failure == TokenFailure::unavailable;
    return Result<std::string>::failure(
        make_failure(ErrorDomain::signaling,
                     retryable ? ErrorCode::connection_lost : ErrorCode::permission_denied,
                     "Token provider did not supply a usable credential", retryable));
  }
  return Result<std::string>::success(std::move(result->token));
}
}  // namespace livekit::sdk::detail
