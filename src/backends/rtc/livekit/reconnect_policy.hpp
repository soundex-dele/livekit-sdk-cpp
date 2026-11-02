#pragma once
#include "livekit/compat/thread.hpp"
#include "livekit/reconnect.hpp"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include "livekit/result.hpp"

namespace livekit::sdk::detail {
inline Result<void> validate_reconnect(const ReconnectOptions& options) {
  if (options.max_attempts == 0 || options.max_attempts > 1000 ||
      options.resume_attempts > options.max_attempts || options.initial_delay.count() < 0 ||
      options.max_delay < options.initial_delay || options.max_delay > std::chrono::minutes(5) ||
      options.timeout.count() <= 0 || options.timeout > std::chrono::hours(1))
    return Result<void>::failure(
        make_failure(ErrorDomain::signaling, ErrorCode::invalid_argument,
                     "Invalid reconnect attempt, delay or timeout budget"));
  return Result<void>::success();
}

// Equal jitter: [50%, 100%] of capped exponential delay. The caller supplies
// entropy so tests can cover exact deadlines without waiting or random flakes.
inline std::chrono::milliseconds reconnect_delay(const ReconnectOptions& options, size_t attempt,
                                                 double entropy) {
  auto delay = options.initial_delay;
  for (size_t i = 1; i < attempt && delay < options.max_delay; ++i)
    delay = std::min(options.max_delay, delay * 2);
  return std::chrono::milliseconds(
      static_cast<int64_t>(delay.count() * (0.5 + 0.5 * std::clamp(entropy, 0.0, 1.0))));
}

// Session-thread owned; a single outage budget covers resume, fresh join and
// transport establishment. Only confirmed media readiness resets it.
class RecoveryBudget {
 public:
  using Clock = std::chrono::steady_clock;
  RecoveryBudget() = default;

  explicit RecoveryBudget(ReconnectOptions options) : options_(options) {}

  void reset() {
    started_ = false;
    attempts_ = 0;
    deadline_ = Clock::time_point::max();
  }

  void begin() {
    if (!started_) {
      started_ = true;
      deadline_ = Clock::now() + options_.timeout;
    }
  }

  Result<size_t> next_attempt() {
    begin();
    if (attempts_ >= options_.max_attempts || Clock::now() >= deadline_)
      return Result<size_t>::failure(make_failure(ErrorDomain::signaling,
                                                  ErrorCode::recovery_failed,
                                                  "Connection recovery budget exhausted"));
    return Result<size_t>::success(++attempts_);
  }

  Result<void> check() const {
    if (started_ && Clock::now() >= deadline_)
      return Result<void>::failure(make_failure(ErrorDomain::signaling, ErrorCode::recovery_failed,
                                                "Connection recovery deadline exceeded"));
    return Result<void>::success();
  }

  Clock::time_point deadline() const {
    return deadline_;
  }

  size_t attempts() const {
    return attempts_;
  }

  bool active() const {
    return started_;
  }

 private:
  ReconnectOptions options_;
  bool started_ = false;
  size_t attempts_ = 0;
  Clock::time_point deadline_ = Clock::time_point::max();
};

inline bool wait_reconnect(livekit::compat::stop_token stop,
                           RecoveryBudget::Clock::time_point until) {
  std::mutex mutex;
  livekit::compat::condition_variable_any changed;
  std::unique_lock lock(mutex);
  changed.wait_until(lock, stop, until, [] { return false; });
  return !stop.stop_requested();
}

inline bool terminal_leave(int reason) {
  // Explicit lifecycle/authorization decisions must never resurrect a session.
  return reason == 1 || reason == 2 || reason == 4 || reason == 5 || reason == 10 || reason == 11 ||
         reason == 12 || reason == 13 || reason == 16;
}

inline Failure rejoin_required(std::string message, int reason = 0) {
  return make_failure(ErrorDomain::signaling, ErrorCode::connection_lost, std::move(message), true,
                      reason);
}
}  // namespace livekit::sdk::detail
