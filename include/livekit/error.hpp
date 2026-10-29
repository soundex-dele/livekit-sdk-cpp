#pragma once
#include <stdexcept>
#include <string>
#include <utility>

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
  request_failed
};

// Backend codes (HTTP status, LiveKit reason, etc.) retain their original value.
class SDKError : public std::runtime_error {
 public:
  SDKError(ErrorDomain domain, ErrorCode code, std::string message, bool retryable = false,
           int backend_reason = 0)
      : std::runtime_error(std::move(message)),
        domain_(domain),
        code_(code),
        retryable_(retryable),
        backend_reason_(backend_reason) {}

  ErrorDomain domain() const noexcept {
    return domain_;
  }

  ErrorCode code() const noexcept {
    return code_;
  }

  bool retryable() const noexcept {
    return retryable_;
  }

  int backend_reason() const noexcept {
    return backend_reason_;
  }

 private:
  ErrorDomain domain_;
  ErrorCode code_;
  bool retryable_;
  int backend_reason_;
};
}  // namespace livekit::sdk
