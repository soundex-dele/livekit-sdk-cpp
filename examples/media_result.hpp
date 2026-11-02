#pragma once
#include "livekit/result.hpp"
#include <iostream>

namespace livekit::sdk::examples {
template <class T>
bool report_result(const Result<T>& result) {
  if (result) {
    return true;
  }
  std::cerr << result.error().message << '\n';
  return false;
}

inline bool report_result(ErrorCode code) {
  if (code == ErrorCode::ok) {
    return true;
  }
  std::cerr << "RTC operation failed: " << static_cast<int>(code) << '\n';
  return false;
}
}  // namespace livekit::sdk::examples
