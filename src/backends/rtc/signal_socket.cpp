#include "backends/rtc/signal_socket.hpp"
#include "livekit/version.hpp"
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <thread>

namespace lk {
using livekit::sdk::ErrorCode;
using livekit::sdk::ErrorDomain;
using livekit::sdk::SDKError;

static void initialize_network() {
  static const bool ready = ix::initNetSystem();
  if (!ready)
    throw std::runtime_error("Network initialization failed");
}

std::vector<std::string> fetch_regions(const std::string& url, const std::string& token,
                                       std::stop_token stop,
                                       std::chrono::steady_clock::time_point deadline) {
  using Clock = std::chrono::steady_clock;
  initialize_network();
  if (stop.stop_requested())
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::cancelled, "Region discovery cancelled");
  if (Clock::now() >= deadline)
    throw SDKError(ErrorDomain::signaling, ErrorCode::timeout, "Region discovery deadline exceeded",
                   true);
  ix::HttpClient client;
  auto args = client.createRequest();
  args->connectTimeout = 30;
  args->transferTimeout = 30;
  args->followRedirects = false;
  args->compress = false;
  args->extraHeaders = {{"Authorization", "Bearer " + token}};
  std::string body;
  bool overflow = false;
  args->onChunkCallback = [&](const std::string& chunk) {
    if (chunk.size() > 65536 - body.size()) {
      overflow = true;
      args->cancel = true;
    } else
      body += chunk;
  };
  std::stop_callback cancel(stop, [&] { args->cancel = true; });
  std::jthread timer([args, deadline](std::stop_token timer_stop) {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::unique_lock lock(mutex);
    changed.wait_until(lock, timer_stop, deadline, [] { return false; });
    if (!timer_stop.stop_requested())
      args->cancel = true;
  });
  auto response = client.get(endpoint(url, false) + "/settings/regions", args);
  timer.request_stop();
  if (stop.stop_requested())
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::cancelled, "Region discovery cancelled");
  if (Clock::now() >= deadline)
    throw SDKError(ErrorDomain::signaling, ErrorCode::timeout, "Region discovery deadline exceeded",
                   true);
  if (overflow)
    throw SDKError(ErrorDomain::protocol, ErrorCode::resource_exhausted,
                   "Region response exceeds limit");
  if (response->errorCode != ix::HttpErrorCode::Ok || response->statusCode != 200)
    throw SDKError(ErrorDomain::signaling, ErrorCode::connection_lost,
                   "Region discovery request failed", true, response->statusCode);
  try {
    const auto settings = Json::parse(body);
    const auto& regions = settings.at("regions");
    if (!regions.is_array() || regions.size() > 32)
      throw std::runtime_error("region count");
    std::vector<std::string> result;
    for (const auto& region : regions)
      result.push_back(region.at("url").get<std::string>());
    return result;
  } catch (...) {
    throw SDKError(ErrorDomain::protocol, ErrorCode::malformed_message,
                   "Malformed region settings");
  }
}

SignalSocket::SignalSocket() {
  initialize_network();
  socket_.disableAutomaticReconnection();
  socket_.disablePerMessageDeflate();
  socket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
    std::lock_guard lock(mutex_);
    switch (message->type) {
      case ix::WebSocketMessageType::Open:
        opened_ = true;
        break;
      case ix::WebSocketMessageType::Message:
        if (messages_.size() >= 1024 || message->str.size() > 4 * 1024 * 1024) {
          error_ = "Signal queue or message size limit exceeded";
          overflow_ = true;
        } else
          messages_.push_back(message->str);
        break;
      case ix::WebSocketMessageType::Error:
        if (!closing_) {
          http_status_ = message->errorInfo.http_status;
          error_ = "WebSocket error (HTTP " + std::to_string(http_status_) + ")";
        }
        break;
      case ix::WebSocketMessageType::Close:
        if (!closing_)
          error_ = "Signal connection closed: " + std::to_string(message->closeInfo.code) + " " +
                   message->closeInfo.reason;
        break;
      default:
        break;
    }
    cv_.notify_all();
  });
}

SignalSocket::~SignalSocket() {
  close();
}

void SignalSocket::connect(const std::string& url, const std::string& token, bool subscribe,
                           std::stop_token stop, int timeout_seconds, const std::string& resume_sid,
                           std::chrono::steady_clock::time_point deadline_limit, int protocol,
                           const std::string& join_request) {
  close();  // joins the old reader before resetting its state
  {
    std::lock_guard lock(mutex_);
    opened_ = false;
    closing_ = false;
    error_.clear();
    messages_.clear();
    http_status_ = 0;
    overflow_ = false;
  }
  if (stop.stop_requested())
    return;
  if (std::chrono::steady_clock::now() >= deadline_limit)
    throw SDKError(ErrorDomain::signaling, ErrorCode::timeout, "Connection deadline exceeded",
                   true);
  // Bearer header avoids leaking tokens through URLs and access logs.
  auto address = endpoint(url, true) + "/rtc?protocol=" + std::to_string(protocol) +
                 "&sdk=cpp&version=" + std::string(livekit::sdk::version) +
                 "&auto_subscribe=" + (subscribe ? "1" : "0");
  if (!resume_sid.empty())
    address += "&reconnect=1&sid=" + percent_encode(resume_sid);
  if (!join_request.empty())
    address = endpoint(url, true) + "/rtc/v1?join_request=" + percent_encode(join_request);
  socket_.setUrl(address);
  socket_.setExtraHeaders({{"Authorization", "Bearer " + token}});
  socket_.setHandshakeTimeout(timeout_seconds);
  socket_.start();
  std::unique_lock lock(mutex_);
  auto deadline = std::min(
      deadline_limit, std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds));
  while (!opened_ && error_.empty() && !stop.stop_requested()) {
    cv_.wait_for(lock, std::chrono::milliseconds(50));
    if (std::chrono::steady_clock::now() >= deadline)
      throw SDKError(ErrorDomain::signaling, ErrorCode::timeout, "WebSocket connect timeout", true);
  }
  if (stop.stop_requested())
    return;
  if (!error_.empty())
    throw SDKError(
        ErrorDomain::signaling,
        http_status_ == 401 || http_status_ == 403 ? ErrorCode::permission_denied
                                                   : ErrorCode::connection_lost,
        error_,
        http_status_ == 0 || http_status_ == 408 || http_status_ == 429 || http_status_ >= 500,
        http_status_);
}

void SignalSocket::send(const std::string& bytes) {
  if (!socket_.sendBinary(bytes).success)
    throw SDKError(ErrorDomain::signaling, ErrorCode::connection_lost, "Signal send failed", true);
}

std::optional<std::string> SignalSocket::receive(int timeout_ms) {
  std::unique_lock lock(mutex_);
  cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
               [&] { return !messages_.empty() || !error_.empty(); });
  if (!messages_.empty()) {
    auto message = std::move(messages_.front());
    messages_.pop_front();
    return message;
  }
  if (!error_.empty())
    throw SDKError(ErrorDomain::signaling,
                   overflow_ ? ErrorCode::resource_exhausted : ErrorCode::connection_lost, error_,
                   !overflow_, http_status_);
  return std::nullopt;
}

void SignalSocket::close() {
  {
    std::lock_guard lock(mutex_);
    closing_ = true;
  }
  socket_.stop();
}
}  // namespace lk
