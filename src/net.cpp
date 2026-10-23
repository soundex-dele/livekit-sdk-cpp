#include "lk/net.hpp"
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <chrono>
#include <stdexcept>
namespace lk {
static void initialize_network() {
  static const bool ready = ix::initNetSystem();
  if (!ready) throw std::runtime_error("Network initialization failed");
}
Json room_api(const std::string& url, const std::string& token, const std::string& method, const Json& request) {
  initialize_network();
  ix::HttpClient client;
  auto args = client.createRequest(); args->connectTimeout = 10; args->transferTimeout = 30;
  args->followRedirects = false; args->compress = false;
  args->extraHeaders = {{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}};
  auto response = client.post(endpoint(url, false) + "/twirp/livekit.RoomService/" + method, request.dump(), args);
  if (response->errorCode != ix::HttpErrorCode::Ok) throw std::runtime_error("Room API transport error: " + response->errorMsg);
  if (response->statusCode < 200 || response->statusCode >= 300) throw std::runtime_error("Room API HTTP " + std::to_string(response->statusCode) + ": " + response->body);
  return Json::parse(response->body.empty() ? "{}" : response->body);
}
SignalSocket::SignalSocket() {
  initialize_network();
  socket_.disableAutomaticReconnection();
  socket_.disablePerMessageDeflate();
  socket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
    std::lock_guard lock(mutex_);
    switch (message->type) {
      case ix::WebSocketMessageType::Open: opened_ = true; break;
      case ix::WebSocketMessageType::Message:
        if (messages_.size() >= 1024 || message->str.size() > 4 * 1024 * 1024) error_ = "Signal queue or message size limit exceeded";
        else messages_.push_back(message->str);
        break;
      case ix::WebSocketMessageType::Error: if (!closing_) error_ = "WebSocket error (HTTP " + std::to_string(message->errorInfo.http_status) + "): " + message->errorInfo.reason; break;
      case ix::WebSocketMessageType::Close: if (!closing_) error_ = "Signal connection closed: " + std::to_string(message->closeInfo.code) + " " + message->closeInfo.reason; break;
      default: break;
    }
    cv_.notify_all();
  });
}
SignalSocket::~SignalSocket() { close(); }
void SignalSocket::connect(const std::string& url, const std::string& token, bool subscribe, std::stop_token stop, int timeout_seconds) {
  // Bearer header avoids leaking tokens through URLs and access logs.
  socket_.setUrl(endpoint(url, true) + "/rtc?protocol=12&sdk=cpp&version=0.2.0&auto_subscribe=" + (subscribe ? "1" : "0"));
  socket_.setExtraHeaders({{"Authorization", "Bearer " + token}});
  socket_.setHandshakeTimeout(timeout_seconds); socket_.start();
  std::unique_lock lock(mutex_);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  while (!opened_ && error_.empty() && !stop.stop_requested()) {
    cv_.wait_for(lock, std::chrono::milliseconds(50));
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("WebSocket connect timeout");
  }
  if (stop.stop_requested()) return;
  if (!error_.empty()) throw std::runtime_error(error_);
}
void SignalSocket::send(const std::string& bytes) {
  if (!socket_.sendBinary(bytes).success) throw std::runtime_error("Signal send failed");
}
std::optional<std::string> SignalSocket::receive(int timeout_ms) {
  std::unique_lock lock(mutex_);
  cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !messages_.empty() || !error_.empty(); });
  if (!messages_.empty()) { auto message = std::move(messages_.front()); messages_.pop_front(); return message; }
  if (!error_.empty()) throw std::runtime_error(error_);
  return std::nullopt;
}
void SignalSocket::close() { { std::lock_guard lock(mutex_); closing_ = true; } socket_.stop(); }
}
