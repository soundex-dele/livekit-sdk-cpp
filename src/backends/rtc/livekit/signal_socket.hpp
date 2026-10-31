#pragma once
#include "livekit/json.hpp"
#include "livekit/encoding.hpp"
#include "livekit/result.hpp"
#include "support/wake_event.hpp"
#include <stop_token>
#include <ixwebsocket/IXWebSocket.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace lk {
using livekit::sdk::endpoint;
using livekit::sdk::Json;
using livekit::sdk::percent_encode;
using livekit::sdk::Result;
Result<std::vector<std::string>> fetch_regions(const std::string& url, const std::string& token,
                                               std::stop_token stop,
                                               std::chrono::steady_clock::time_point deadline);

class SignalSocket {
 public:
  explicit SignalSocket(std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup = {});
  ~SignalSocket();
  Result<void> connect(const std::string& url, const std::string& token, bool subscribe,
                       std::stop_token stop, int timeout_seconds,
                       const std::string& resume_sid = {},
                       std::chrono::steady_clock::time_point deadline_limit =
                           std::chrono::steady_clock::time_point::max(),
                       int protocol = 12, const std::string& join_request = {});
  Result<void> send(const std::string& bytes);
  Result<std::optional<std::string>> receive(int timeout_ms);
  Result<std::optional<std::string>> receive_until(std::chrono::steady_clock::time_point,
                                                   std::stop_token = {});
  Result<std::optional<std::string>> take_message();
  void close();

 private:
  ix::WebSocket socket_;
  std::mutex mutex_;
  std::condition_variable_any cv_;
  const std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup_;
  std::deque<std::string> messages_;
  std::string error_;
  int http_status_ = 0;
  bool overflow_ = false;
  bool opened_ = false;
  bool closing_ = false;
};
}  // namespace lk
