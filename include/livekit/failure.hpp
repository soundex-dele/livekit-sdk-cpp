#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace livekit::sdk {
enum class ErrorDomain { signaling, transport, protocol, lifecycle, media, data };
enum class ErrorCode {
  ok = 0,
  connection_lost,
  timeout,
  cancelled,
  permission_denied,
  malformed_message,
  resource_exhausted,
  stale_handle,
  session_closed,
  recovery_failed,
  unsupported,
  outcome_unknown,
  request_failed,
  invalid_argument,
  processing_failed,
  callback_failed,
  invalid_thread,
  invalid_state,
  internal_error,
  busy
};

struct FailureContext {
  std::string operation;
  std::string track_id;
  std::string node;
  uint64_t epoch = 0;
};

// Correlation survives propagation through results, callbacks and snapshots.
// Consumers deduplicate by id; context may be enriched without changing it.
struct FailureInfo {
  uint64_t id = 0;
  ErrorDomain domain = ErrorDomain::lifecycle;
  ErrorCode code = ErrorCode::internal_error;
  std::string message;
  bool retryable = false;
  int backend_reason = 0;
  FailureContext context;
  std::string backend_code;
  std::string backend_metadata;
  std::string backend_data;
};

inline uint64_t next_failure_id() {
  static std::atomic<uint64_t> next{0};
  return ++next;
}

using Failure = std::shared_ptr<const FailureInfo>;

inline Failure make_failure(ErrorDomain domain, ErrorCode code, std::string message,
                            bool retryable = false, int backend_reason = 0,
                            FailureContext context = {}) {
  return std::make_shared<const FailureInfo>(FailureInfo{next_failure_id(), domain, code,
                                                         std::move(message), retryable,
                                                         backend_reason, std::move(context)});
}

inline Failure with_context(Failure failure, FailureContext context) {
  if (!failure) {
    return failure;
  }
  auto result = *failure;
  if (result.context.operation.empty()) {
    result.context.operation = std::move(context.operation);
  }
  if (!context.track_id.empty()) {
    result.context.track_id = std::move(context.track_id);
  }
  if (result.context.node.empty()) {
    result.context.node = std::move(context.node);
  }
  if (context.epoch) {
    result.context.epoch = context.epoch;
  }
  return std::make_shared<const FailureInfo>(std::move(result));
}
}  // namespace livekit::sdk
