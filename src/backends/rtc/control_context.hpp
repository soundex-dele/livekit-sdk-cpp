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

template <class Function>
auto control_boundary(Function&& function) {
  using Value = std::invoke_result_t<Function>;
  try {
    if constexpr (std::is_void_v<Value>) {
      function();
      return ErrorCode::ok;
    } else {
      return function();
    }
  } catch (...) {
    if constexpr (std::is_void_v<Value> || std::is_same_v<Value, ErrorCode>) {
      return describe_failure(std::current_exception())->code;
    } else {
      return Value::failure(describe_failure(std::current_exception()));
    }
  }
}
}  // namespace livekit::sdk::rtc::detail
