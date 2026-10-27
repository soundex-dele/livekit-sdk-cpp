#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

namespace livekit::sdk {
enum class RpcErrorCode : uint32_t {
  unsupported_method=1400, recipient_not_found=1401, request_payload_too_large=1402,
  unsupported_server=1403, unsupported_version=1404,
  application_error=1500, connection_timeout=1501, response_timeout=1502,
  recipient_disconnected=1503, response_payload_too_large=1504, send_failed=1505
};
class RpcError : public std::runtime_error {
 public:
  RpcError(uint32_t code,std::string message,std::string data={})
      :std::runtime_error(std::move(message)),code_(code),data_(std::move(data)){}
  RpcError(RpcErrorCode code,std::string message,std::string data={})
      :RpcError(static_cast<uint32_t>(code),std::move(message),std::move(data)){}
  uint32_t code() const noexcept{return code_;}
  const std::string& data() const noexcept{return data_;}
 private:
  uint32_t code_;
  std::string data_;
};
struct RpcCallOptions {
  std::string destination_identity,method,payload;
  std::chrono::milliseconds response_timeout{15000};
  std::stop_token stop;
};
struct RpcInvocation {
  std::string request_id,caller_identity,payload;
  std::chrono::steady_clock::time_point deadline;
  std::stop_token stop;
};
struct RpcResult {std::string payload;std::optional<RpcError> error;};
using RpcCompletion=std::function<void(RpcResult)>;
// Runs on an independent, bounded per-Room dispatcher. Start asynchronous work
// and return promptly. Completion may run on any thread, at most once is used.
// Observe stop/deadline and own captures: late application work may outlive Room.
using RpcHandler=std::function<void(RpcInvocation,RpcCompletion)>;
}
