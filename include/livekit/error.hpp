#pragma once
#include <stdexcept>
#include <string>
#include <utility>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>

namespace livekit::sdk {
enum class ErrorDomain { signaling, transport, protocol, lifecycle, media, data };
enum class ErrorCode {
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
  internal_error
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
};

inline uint64_t next_failure_id() {
  static std::atomic<uint64_t> next{0};
  return ++next;
}

// Backend codes (HTTP status, LiveKit reason, etc.) retain their original value.
class SDKError : public std::runtime_error {
 public:
  SDKError(ErrorDomain domain, ErrorCode code, std::string message, bool retryable = false,
           int backend_reason = 0)
      : SDKError(std::make_shared<const FailureInfo>(FailureInfo{
            next_failure_id(), domain, code, std::move(message), retryable, backend_reason, {}})) {}

  explicit SDKError(std::shared_ptr<const FailureInfo> failure)
      : std::runtime_error(failure ? failure->message
                                   : throw std::invalid_argument("Null failure record")),
        failure_(std::move(failure)) {}

  const std::shared_ptr<const FailureInfo>& failure() const noexcept {
    return failure_;
  }

  ErrorDomain domain() const noexcept {
    return failure_->domain;
  }

  ErrorCode code() const noexcept {
    return failure_->code;
  }

  bool retryable() const noexcept {
    return failure_->retryable;
  }

  int backend_reason() const noexcept {
    return failure_->backend_reason;
  }

 private:
  std::shared_ptr<const FailureInfo> failure_;
};

inline std::shared_ptr<const FailureInfo> describe_failure(
    std::exception_ptr cause, FailureContext context = {},
    ErrorDomain domain = ErrorDomain::lifecycle, ErrorCode code = ErrorCode::internal_error) {
  FailureInfo result;
  result.domain = domain;
  result.code = code;
  result.message = "Unknown SDK failure";
  try {
    if (cause) {
      std::rethrow_exception(cause);
    }
  } catch (const SDKError& error) {
    result = *error.failure();
  } catch (const std::invalid_argument& error) {
    result.code = ErrorCode::invalid_argument;
    result.message = error.what();
  } catch (const std::logic_error& error) {
    result.code = ErrorCode::invalid_state;
    result.message = error.what();
  } catch (const std::exception& error) {
    result.message = error.what();
  } catch (...) {
  }
  if (!result.id) {
    result.id = next_failure_id();
  }
  if (result.context.operation.empty() && !context.operation.empty()) {
    result.context.operation = std::move(context.operation);
  }
  if (!context.track_id.empty()) {
    result.context.track_id = std::move(context.track_id);
  }
  if (result.context.node.empty() && !context.node.empty()) {
    result.context.node = std::move(context.node);
  }
  if (context.epoch) {
    result.context.epoch = context.epoch;
  }
  return std::make_shared<const FailureInfo>(std::move(result));
}

inline std::exception_ptr normalized_failure(std::exception_ptr cause, FailureContext context = {},
                                             ErrorDomain domain = ErrorDomain::lifecycle,
                                             ErrorCode code = ErrorCode::internal_error) {
  return std::make_exception_ptr(
      SDKError(describe_failure(cause, std::move(context), domain, code)));
}
}  // namespace livekit::sdk
