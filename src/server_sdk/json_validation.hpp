#pragma once
#include "livekit/json.hpp"
#include "livekit/result.hpp"
#include "support/utf8.hpp"

namespace livekit::sdk::detail {
inline bool valid_json_text(const Json& value, size_t depth = 0) {
  if (depth > 128 || value.is_discarded()) {
    return false;
  }
  if (value.is_string()) {
    return valid_utf8(value.get_ref<const std::string&>());
  }
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (!valid_utf8(it.key()) || !valid_json_text(it.value(), depth + 1)) {
        return false;
      }
    }
  } else if (value.is_array()) {
    for (const auto& item : value) {
      if (!valid_json_text(item, depth + 1)) {
        return false;
      }
    }
  }
  return true;
}

inline Result<std::string> serialize_json(const Json& value) {
  if (!valid_json_text(value)) {
    return Result<std::string>::failure(
        make_failure(ErrorDomain::protocol, ErrorCode::invalid_argument,
                     "JSON contains invalid UTF-8, discarded values or excessive nesting", false, 0,
                     {"serialize_json"}));
  }
  return Result<std::string>::success(value.dump());
}
}  // namespace livekit::sdk::detail
