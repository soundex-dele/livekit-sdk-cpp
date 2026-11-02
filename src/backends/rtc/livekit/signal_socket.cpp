#include "livekit/compat/thread.hpp"
#include "backends/rtc/livekit/signal_socket.hpp"
#include "livekit/version.hpp"
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <chrono>
#include <algorithm>
#include "backends/rtc/livekit/region_policy.hpp"
#include <thread>

namespace lk {
using livekit::sdk::ErrorCode;
using livekit::sdk::ErrorDomain;
using livekit::sdk::make_failure;

static bool initialize_network() {
  static const bool ready = ix::initNetSystem();
  return ready;
}

Result<std::vector<std::string>> fetch_regions(const std::string& url, const std::string& token,
                                               livekit::compat::stop_token stop,
                                               std::chrono::steady_clock::time_point deadline) {
  using Clock = std::chrono::steady_clock;
  if (!initialize_network()) {
    return Result<std::vector<std::string>>::failure(make_failure(
        ErrorDomain::transport, ErrorCode::processing_failed, "Network initialization failed"));
  }
  auto parsed = livekit::sdk::detail::signal_endpoint(url);
  if (!parsed) {
    return Result<std::vector<std::string>>::failure(parsed.failure_record());
  }
  const auto address =
      std::string(parsed->secure ? "https" : "http") + parsed->url.substr(parsed->url.find("://"));
  if (stop.stop_requested())
    return Result<std::vector<std::string>>::failure(
        make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled, "Region discovery cancelled"));
  if (Clock::now() >= deadline)
    return Result<std::vector<std::string>>::failure(make_failure(
        ErrorDomain::signaling, ErrorCode::timeout, "Region discovery deadline exceeded", true));
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
  livekit::compat::stop_callback cancel(stop, [&] { args->cancel = true; });
  livekit::compat::jthread timer([args, deadline](livekit::compat::stop_token timer_stop) {
    std::mutex mutex;
    livekit::compat::condition_variable_any changed;
    std::unique_lock lock(mutex);
    changed.wait_until(lock, timer_stop, deadline, [] { return false; });
    if (!timer_stop.stop_requested())
      args->cancel = true;
  });
  auto response = client.get(address + "/settings/regions", args);
  timer.request_stop();
  if (stop.stop_requested())
    return Result<std::vector<std::string>>::failure(
        make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled, "Region discovery cancelled"));
  if (Clock::now() >= deadline)
    return Result<std::vector<std::string>>::failure(make_failure(
        ErrorDomain::signaling, ErrorCode::timeout, "Region discovery deadline exceeded", true));
  if (overflow)
    return Result<std::vector<std::string>>::failure(make_failure(
        ErrorDomain::protocol, ErrorCode::resource_exhausted, "Region response exceeds limit"));
  if (response->errorCode != ix::HttpErrorCode::Ok || response->statusCode != 200)
    return Result<std::vector<std::string>>::failure(
        make_failure(ErrorDomain::signaling, ErrorCode::connection_lost,
                     "Region discovery request failed", true, response->statusCode));
  const auto settings = Json::parse(body, nullptr, false);
  if (!settings.is_object() || !settings.contains("regions") || !settings["regions"].is_array() ||
      settings["regions"].size() > 32) {
    return Result<std::vector<std::string>>::failure(make_failure(
        ErrorDomain::protocol, ErrorCode::malformed_message, "Malformed region settings"));
  }
  std::vector<std::string> result;
  for (const auto& region : settings["regions"]) {
    if (!region.is_object() || !region.contains("url") || !region["url"].is_string()) {
      return Result<std::vector<std::string>>::failure(make_failure(
          ErrorDomain::protocol, ErrorCode::malformed_message, "Malformed region endpoint"));
    }
    result.push_back(region["url"].get<std::string>());
  }
  return Result<std::vector<std::string>>::success(std::move(result));
}

SignalSocket::SignalSocket(std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup)
    : wakeup_(std::move(wakeup)) {
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
    if (wakeup_) {
      wakeup_->signal();
    }
  });
}

SignalSocket::~SignalSocket() {
  close();
}

Result<void> SignalSocket::connect(const std::string& url, const std::string& token, bool subscribe,
                                   livekit::compat::stop_token stop, int timeout_seconds,
                                   const std::string& resume_sid,
                                   std::chrono::steady_clock::time_point deadline_limit,
                                   int protocol, const std::string& join_request) {
  if (!initialize_network()) {
    return Result<void>::failure(make_failure(ErrorDomain::transport, ErrorCode::processing_failed,
                                              "Network initialization failed"));
  }
  auto parsed = livekit::sdk::detail::signal_endpoint(url);
  if (!parsed) {
    return Result<void>::failure(parsed.failure_record());
  }
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
    return Result<void>::failure(
        make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled, "Signal connection cancelled"));
  if (std::chrono::steady_clock::now() >= deadline_limit)
    return Result<void>::failure(make_failure(ErrorDomain::signaling, ErrorCode::timeout,
                                              "Connection deadline exceeded", true));
  // Bearer header avoids leaking tokens through URLs and access logs.
  auto address = parsed->url + "/rtc?protocol=" + std::to_string(protocol) +
                 "&sdk=cpp&version=" + std::string(livekit::sdk::version) +
                 "&auto_subscribe=" + (subscribe ? "1" : "0");
  if (!resume_sid.empty())
    address += "&reconnect=1&sid=" + percent_encode(resume_sid);
  if (!join_request.empty())
    address = parsed->url + "/rtc/v1?join_request=" + percent_encode(join_request);
  socket_.setUrl(address);
  socket_.setExtraHeaders({{"Authorization", "Bearer " + token}});
  socket_.setHandshakeTimeout(timeout_seconds);
  socket_.start();
  std::unique_lock lock(mutex_);
  auto deadline = std::min(
      deadline_limit, std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds));
  const bool completed =
      cv_.wait_until(lock, stop, deadline, [&] { return opened_ || !error_.empty() || closing_; });
  if (stop.stop_requested())
    return Result<void>::failure(
        make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled, "Signal connection cancelled"));
  if (!completed) {
    return Result<void>::failure(make_failure(ErrorDomain::signaling, ErrorCode::timeout,
                                              "WebSocket connect timeout", true));
  }
  if (!error_.empty())
    return Result<void>::failure(make_failure(
        ErrorDomain::signaling,
        http_status_ == 401 || http_status_ == 403 ? ErrorCode::permission_denied
                                                   : ErrorCode::connection_lost,
        error_,
        http_status_ == 0 || http_status_ == 408 || http_status_ == 429 || http_status_ >= 500,
        http_status_));
  return Result<void>::success();
}

Result<void> SignalSocket::send(const std::string& bytes) {
  if (!socket_.sendBinary(bytes).success)
    return Result<void>::failure(make_failure(ErrorDomain::signaling, ErrorCode::connection_lost,
                                              "Signal send failed", true));
  return Result<void>::success();
}

Result<std::optional<std::string>> SignalSocket::receive(int timeout_ms) {
  return receive_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
}

Result<std::optional<std::string>> SignalSocket::take_message() {
  return receive_until(std::chrono::steady_clock::now());
}

Result<std::optional<std::string>> SignalSocket::receive_until(
    std::chrono::steady_clock::time_point deadline, livekit::compat::stop_token stop) {
  std::unique_lock lock(mutex_);
  cv_.wait_until(lock, stop, deadline,
                 [&] { return !messages_.empty() || !error_.empty() || closing_; });
  if (stop.stop_requested()) {
    return Result<std::optional<std::string>>::failure(
        make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled, "Signal receive cancelled"));
  }
  if (!messages_.empty()) {
    auto message = std::move(messages_.front());
    messages_.pop_front();
    return Result<std::optional<std::string>>::success(std::move(message));
  }
  if (!error_.empty())
    return Result<std::optional<std::string>>::failure(
        make_failure(ErrorDomain::signaling,
                     overflow_ ? ErrorCode::resource_exhausted : ErrorCode::connection_lost, error_,
                     !overflow_, http_status_));
  return Result<std::optional<std::string>>::success(std::nullopt);
}

void SignalSocket::close() {
  {
    std::lock_guard lock(mutex_);
    closing_ = true;
    cv_.notify_all();
    if (wakeup_) {
      wakeup_->signal();
    }
  }
  socket_.stop();
}
}  // namespace lk
