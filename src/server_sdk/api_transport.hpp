#pragma once
#include "livekit/api_client.hpp"

namespace livekit::sdk::detail {
inline Failure invalid_request(std::string message) {
  return make_failure(ErrorDomain::protocol, ErrorCode::invalid_argument, std::move(message), false,
                      0, {"server_request"});
}

inline Result<std::string> field(const Json& body, const char* camel, const char* snake = nullptr) {
  auto value = body.find(camel);
  if (value == body.end() && snake) {
    value = body.find(snake);
  }
  if (value == body.end() || !value->is_string() || value->get_ref<const std::string&>().empty()) {
    return Result<std::string>::failure(
        invalid_request(std::string("Missing or invalid ") + camel));
  }
  return Result<std::string>::success(value->get<std::string>());
}

Result<std::string> random_base64(int bytes);
}  // namespace livekit::sdk::detail
