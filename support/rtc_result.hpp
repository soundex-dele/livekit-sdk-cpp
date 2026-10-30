#pragma once
#include "livekit/result.hpp"

namespace livekit::sdk::app_support {
// Application boundary for supervisors/tests whose existing error handler uses
// exceptions. The SDK itself returns completed, structured results.
template <class T>
T require_result(Result<T> result) {
  if (!result) {
    throw SDKError(result.failure_record());
  }
  if constexpr (!std::is_void_v<T>) {
    return *std::move(result);
  }
}
}  // namespace livekit::sdk::app_support
