#pragma once
#include "livekit/result.hpp"
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <utility>

namespace livekit::sdk::app_support {
// Test assertion: production callers must propagate Result explicitly.
template <class T>
T require_result(Result<T> result) {
  if (!result) {
    std::cerr << "Unexpected test failure: " << result.failure_record()->message << '\n';
    std::exit(EXIT_FAILURE);
  }
  if constexpr (!std::is_void_v<T>) {
    return *std::move(result);
  }
}

inline void require_result(ErrorCode code) {
  if (code != ErrorCode::ok) {
    std::cerr << "Unexpected test error: " << static_cast<int>(code) << '\n';
    std::exit(EXIT_FAILURE);
  }
}
}  // namespace livekit::sdk::app_support
