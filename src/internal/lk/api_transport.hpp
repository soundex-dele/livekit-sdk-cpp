#pragma once
#include "livekit/api_client.hpp"
namespace livekit::sdk::detail {
inline Json field(const Json& body, const char* camel, const char* snake) {
  return body.contains(camel) ? body.at(camel) : body.at(snake);
}
std::string random_base64(int bytes);
}
