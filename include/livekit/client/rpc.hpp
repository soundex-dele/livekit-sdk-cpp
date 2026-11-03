#pragma once
#include "livekit/compat/thread.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include "livekit/result.hpp"
#include <string>
#include <utility>

namespace livekit::sdk::client {
enum class RpcErrorCode : uint32_t {
  unsupported_method = 1400,
  recipient_not_found = 1401,
  request_payload_too_large = 1402,
  unsupported_server = 1403,
  unsupported_version = 1404,
  application_error = 1500,
  connection_timeout = 1501,
  response_timeout = 1502,
  recipient_disconnected = 1503,
  response_payload_too_large = 1504,
  send_failed = 1505
};

class RpcError {
 public:
  RpcError(uint32_t code, std::string message, std::string data = {})
      : message_(std::move(message)), code_(code), data_(std::move(data)) {}

  RpcError(RpcErrorCode code, std::string message, std::string data = {})
      : RpcError(static_cast<uint32_t>(code), std::move(message), std::move(data)) {}

  const char* what() const noexcept {
    return message_.c_str();
  }

  uint32_t code() const noexcept {
    return code_;
  }

  const std::string& data() const noexcept {
    return data_;
  }

 private:
  std::string message_;
  uint32_t code_;
  std::string data_;
};

struct RpcCallOptions {
  std::string destination_identity, method, payload;
  std::chrono::milliseconds response_timeout{15000};
  livekit::compat::stop_token stop;
  bool compress = false;  // Opt-in only for peers supporting compressed RPC payloads.
};

struct RpcInvocation {
  std::string request_id, caller_identity, payload;
  std::chrono::steady_clock::time_point deadline;
  livekit::compat::stop_token stop;
};

struct RpcResult {
  std::string payload;
  std::optional<RpcError> error;
};

// Runs on a bounded worker, outside transport and Observer threads. Return by
// invocation.deadline and honor stop; room close cancels and joins handlers.
// Do not close or destroy the owning Room/MediaEngine from a handler.
using RpcHandler = std::function<RpcResult(RpcInvocation)>;
}  // namespace livekit::sdk::client
