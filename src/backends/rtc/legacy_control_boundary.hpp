#pragma once
#include "backends/rtc/control_context.hpp"
#include "livekit/error.hpp"

namespace livekit::sdk::rtc::detail {
// Temporary adapter for media/data implementations still being migrated.
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
