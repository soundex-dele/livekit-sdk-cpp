#pragma once
#include "backends/rtc/control_context.hpp"
#include <type_traits>
#include <utility>

namespace livekit::sdk::rtc::detail {
// Preserve the old call shape while implementations migrate to explicit results.
// This adapter performs no exception conversion; callbacks report their failure.
template <class Function>
auto control_boundary(Function&& function) {
  using Value = std::invoke_result_t<Function>;
  if constexpr (std::is_void_v<Value>) {
    std::forward<Function>(function)();
    return ErrorCode::ok;
  } else {
    return std::forward<Function>(function)();
  }
}
}  // namespace livekit::sdk::rtc::detail
