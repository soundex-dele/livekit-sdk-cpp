#pragma once
#include "livekit/result.hpp"

namespace livekit::sdk::app_support {
// Application boundary for supervisors/tests whose existing error handler uses
// exceptions. Value results retain structured failure details.
template <class T>
T require_result(Result<T> result) {
  if (!result) {
    throw SDKError(result.failure_record());
  }
  return *std::move(result);
}

// Legacy application supervisors still use exceptions locally. Engine calls
// return ErrorCode/Result<T>; this build-tree helper is not part of the SDK API.
inline void require_result(ErrorCode code) {
  if (code != ErrorCode::ok) {
    throw SDKError(ErrorDomain::lifecycle, code, "RTC operation failed");
  }
}

}  // namespace livekit::sdk::app_support
