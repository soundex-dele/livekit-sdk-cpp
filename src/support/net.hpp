#pragma once
#include "server_sdk/auth.hpp"
#include <stop_token>
#include <ixwebsocket/IXWebSocket.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
namespace lk {
Json room_api(const std::string& url, const std::string& token,
              const std::string& method, const Json& request);
class SignalSocket {
 public:
  SignalSocket();
  ~SignalSocket();
  void connect(const std::string& url, const std::string& token, bool subscribe, std::stop_token stop, int timeout_seconds);
  void send(const std::string& bytes);
  std::optional<std::string> receive(int timeout_ms);
  void close();
 private:
  ix::WebSocket socket_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> messages_;
  std::string error_;
  bool opened_ = false;
  bool closing_ = false;
};
}
