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
// return codes/values; this build-tree helper is not part of the SDK API.
inline void require_result(ErrorCode code) {
  if (code != ErrorCode::ok) {
    throw SDKError(ErrorDomain::lifecycle, code, "RTC operation failed");
  }
}

template <class T>
T require_result(T value) {
  bool valid;
  if constexpr (requires { value.empty(); }) {
    valid = !value.empty();
  } else if constexpr (requires { value.track; }) {
    valid = bool(value.track);
  } else {
    valid = bool(value);
  }
  if (!valid) {
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::request_failed, "RTC operation failed");
  }
  return value;
}
}  // namespace livekit::sdk::app_support
