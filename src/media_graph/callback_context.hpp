#pragma once

#include "livekit/result.hpp"

namespace livekit::sdk::rtc::detail {
// Scoped on every user callback and RTC worker. Never acquire an operation lock there.
inline thread_local unsigned callback_depth = 0;

struct CallbackScope {
  CallbackScope() {
    ++callback_depth;
  }

  ~CallbackScope() {
    --callback_depth;
  }
};

template <class T>
Result<T> failure_result(ErrorCode code, std::string message) {
  return Result<T>::failure(std::make_shared<FailureInfo>(
      FailureInfo{next_failure_id(), ErrorDomain::lifecycle, code, std::move(message)}));
}

}  // namespace livekit::sdk::rtc::detail
