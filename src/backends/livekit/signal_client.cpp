#include "backends/livekit/signal_client.hpp"
#include <ixwebsocket/IXNetSystem.h>
#include <cctype>

namespace livekit::sdk::client::detail {
namespace {
std::string escape(const std::string& input) {
  static const char* hex = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : input) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      result += char(c);
    } else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 15];
    }
  }
  return result;
}
}  // namespace

SignalClient::~SignalClient() {
  close();
}

Result<void> SignalClient::connect(const ConnectionOptions& options) {
  static std::once_flag network;
  std::call_once(network, [] { ix::initNetSystem(); });
  auto url = options.url;
  if (url.rfind("https://", 0) == 0) {
    url.replace(0, 8, "wss://");
  }
  if (url.rfind("http://", 0) == 0) {
    url.replace(0, 7, "ws://");
  }
  if ((url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0) ||
      url.find_first_of("?#") != std::string::npos) {
    return Result<void>::failure(
        make_failure(ErrorDomain::signaling, ErrorCode::invalid_argument,
                     "Expected a ws(s)/http(s) server URL without query or fragment"));
  }
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  if (url.size() < 4 || url.substr(url.size() - 4) != "/rtc") {
    url += "/rtc";
  }
  url += "?access_token=" + escape(options.token) +
         "&protocol=15&sdk=cpp&version=0.3.0&auto_subscribe=0";
  max_events_ = options.max_events;
  socket_.disableAutomaticReconnection();
  socket_.setUrl(url);
  socket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
    std::lock_guard lock(mutex_);
    if (message->type == ix::WebSocketMessageType::Message) {
      if (message->str.size() > 4 * 1024 * 1024 || messages_.size() >= max_events_ ||
          bytes_ + message->str.size() > 8 * 1024 * 1024) {
        failure_ = make_failure(ErrorDomain::signaling, ErrorCode::resource_exhausted,
                                "Signaling inbox exceeded its bound");
        return;
      }
      livekit::SignalResponse response;
      if (!response.ParseFromString(message->str)) {
        failure_ = make_failure(ErrorDomain::protocol, ErrorCode::malformed_message,
                                "Invalid signal protobuf");
        return;
      }
      bytes_ += response.ByteSizeLong();
      messages_.push_back(std::move(response));
    } else if (message->type == ix::WebSocketMessageType::Error) {
      // ix error text can contain the authenticated URL. Preserve only HTTP code.
      const auto code = message->errorInfo.http_status;
      failure_ = make_failure(
          ErrorDomain::signaling,
          code == 401 || code == 403 ? ErrorCode::permission_denied : ErrorCode::connection_lost,
          "Signaling WebSocket failed (HTTP " + std::to_string(code) + ")",
          code != 401 && code != 403, code);
    } else if (message->type == ix::WebSocketMessageType::Close) {
      failure_ = make_failure(ErrorDomain::signaling, ErrorCode::connection_lost,
                              "Signaling socket closed", true);
    }
  });
  socket_.start();
  return Result<void>::success();
}

Result<void> SignalClient::send(const livekit::SignalRequest& request) {
  if (auto failure = status(); !failure) {
    return failure;
  }
  if (socket_.bufferedAmount() > 4 * 1024 * 1024) {
    return Result<void>::failure(make_failure(ErrorDomain::signaling, ErrorCode::resource_exhausted,
                                              "Signaling output is full"));
  }
  if (!socket_.sendBinary(request.SerializeAsString()).success) {
    return Result<void>::failure(make_failure(ErrorDomain::signaling, ErrorCode::connection_lost,
                                              "Cannot send signaling request", true));
  }
  return Result<void>::success();
}

std::optional<livekit::SignalResponse> SignalClient::next() {
  std::lock_guard lock(mutex_);
  if (messages_.empty()) {
    return {};
  }
  auto message = std::move(messages_.front());
  messages_.pop_front();
  bytes_ -= message.ByteSizeLong();
  return message;
}

Result<void> SignalClient::status() const {
  std::lock_guard lock(mutex_);
  return failure_ ? Result<void>::failure(failure_) : Result<void>::success();
}

void SignalClient::close() {
  socket_.stop();
}
}  // namespace livekit::sdk::client::detail
