#pragma once

#include "livekit/rtc/completion.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>

namespace livekit::sdk::test {
// Blocking is confined to the test owner. Production native APIs only complete
// through callbacks, which may run inline or on a WebRTC thread.
template <class T>
class PendingCompletion {
 public:
  rtc::Completion<T> callback() const {
    return [state = state_](Result<T> result) {
      std::lock_guard lock(state->mutex);
      if (++state->calls != 1) {
        std::fputs("Native completion delivered more than once\n", stderr);
        std::abort();
      }
      state->result.emplace(std::move(result));
      state->changed.notify_all();
    };
  }

  Result<T> wait() {
    std::unique_lock lock(state_->mutex);
    if (!state_->changed.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return state_->result.has_value(); })) {
      return Result<T>::failure(
          make_failure(ErrorDomain::lifecycle, ErrorCode::timeout, "Native completion timed out"));
    }
    return std::move(*state_->result);
  }

  unsigned calls() const {
    std::lock_guard lock(state_->mutex);
    return state_->calls;
  }

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Result<T>> result;
    unsigned calls = 0;
  };

  std::shared_ptr<State> state_ = std::make_shared<State>();
};

template <class T>
T require_native(Result<T> result) {
  if (!result) {
    std::fprintf(stderr, "Native test operation failed: %s\n", result.error().message.c_str());
    std::abort();
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

template <class T, class Start>
T complete_native(Start start) {
  PendingCompletion<T> pending;
  start(pending.callback());
  return require_native(pending.wait());
}
}  // namespace livekit::sdk::test
