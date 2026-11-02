#pragma once
#include "livekit/result.hpp"

namespace livekit::sdk::studio_detail {
struct Failed {
  Failure error;

  template <class T>
  operator Result<T>() const {
    return Result<T>::failure(error);
  }
};

inline Failed failed(Failure error) {
  return {std::move(error)};
}

inline Failed failed(std::string message, ErrorCode code = ErrorCode::processing_failed,
                     int backend_reason = 0) {
  return {make_failure(ErrorDomain::media, code, std::move(message), false, backend_reason,
                       {"studio"})};
}
}  // namespace livekit::sdk::studio_detail

#define LK_STUDIO_TRY(expression)                     \
  do {                                                \
    auto checked_result = (expression);               \
    if (!checked_result) {                            \
      return failed(checked_result.failure_record()); \
    }                                                 \
  } while (false)
#define LK_STUDIO_ASSIGN(name, expression)         \
  auto name##_result = (expression);               \
  if (!name##_result) {                            \
    return failed(name##_result.failure_record()); \
  }                                                \
  auto name = std::move(*name##_result)
