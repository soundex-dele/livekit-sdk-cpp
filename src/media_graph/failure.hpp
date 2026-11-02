#pragma once
#include "core/graph_node.h"
#include "livekit/result.hpp"
#include <any>

namespace livekit::sdk::detail {
inline graphflow::core::GraphError graph_error(Failure failure) {
  return {failure ? failure->message : "Unknown media graph failure", std::move(failure)};
}

inline Failure graph_failure(const graphflow::core::GraphError& error,
                             FailureContext context = {}) {
  if (const auto* failure = std::any_cast<Failure>(&error.detail); failure && *failure) {
    return with_context(*failure, std::move(context));
  }
  return make_failure(ErrorDomain::media, ErrorCode::processing_failed, error.message, false, 0,
                      std::move(context));
}
}  // namespace livekit::sdk::detail
