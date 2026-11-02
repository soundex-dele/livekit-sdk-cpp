#include "livekit/encoding.hpp"

namespace livekit::sdk {
std::string base64url(std::string_view value) {
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string encoded;
  unsigned bits = 0, count = 0;
  for (unsigned char byte : value) {
    bits = (bits << 8) | byte;
    count += 8;
    while (count >= 6) {
      count -= 6;
      encoded += alphabet[(bits >> count) & 63];
    }
  }
  if (count) {
    encoded += alphabet[(bits << (6 - count)) & 63];
  }
  return encoded;
}

Result<std::string> base64_decode(std::string_view value) {
  auto invalid = [] {
    return Result<std::string>::failure(
        make_failure(ErrorDomain::protocol, ErrorCode::invalid_argument, "Invalid Base64 data",
                     false, 0, {"base64_decode"}));
  };
  const auto first_padding = value.find('=');
  const size_t content_size =
      first_padding == std::string_view::npos ? value.size() : first_padding;
  const size_t padding = value.size() - content_size;
  if (content_size % 4 == 1 || padding > 2 ||
      (padding && (value.size() % 4 || content_size % 4 + padding != 4))) {
    return invalid();
  }
  for (size_t i = content_size; i < value.size(); ++i) {
    if (value[i] != '=') {
      return invalid();
    }
  }
  std::string decoded;
  unsigned bits = 0, count = 0;
  for (size_t i = 0; i < content_size; ++i) {
    const unsigned char c = value[i];
    unsigned digit;
    if (c >= 'A' && c <= 'Z') {
      digit = c - 'A';
    } else if (c >= 'a' && c <= 'z') {
      digit = c - 'a' + 26;
    } else if (c >= '0' && c <= '9') {
      digit = c - '0' + 52;
    } else if (c == '+' || c == '-') {
      digit = 62;
    } else if (c == '/' || c == '_') {
      digit = 63;
    } else {
      return invalid();
    }
    bits = (bits << 6) | digit;
    count += 6;
    if (count >= 8) {
      count -= 8;
      decoded += static_cast<char>((bits >> count) & 255);
    }
  }
  if (count && (bits & ((1u << count) - 1))) {
    return invalid();
  }
  return Result<std::string>::success(std::move(decoded));
}

std::string percent_encode(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~')
      result += c;
    else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 15];
    }
  }
  return result;
}

Result<std::string> endpoint(std::string url, bool ws) {
  auto pos = url.find("://");
  if (pos == std::string::npos)
    return Result<std::string>::failure(
        make_failure(ErrorDomain::protocol, ErrorCode::invalid_argument,
                     "URL must start with http(s):// or ws(s)://", false, 0, {"endpoint"}));
  auto scheme = url.substr(0, pos);
  if (scheme != "http" && scheme != "https" && scheme != "ws" && scheme != "wss")
    return Result<std::string>::failure(
        make_failure(ErrorDomain::protocol, ErrorCode::invalid_argument, "Unsupported URL scheme",
                     false, 0, {"endpoint"}));
  auto rest = url.substr(pos + 3);
  if (rest.empty() || rest.front() == '/' || rest.find_first_of("?#@\r\n") != std::string::npos)
    return Result<std::string>::failure(make_failure(
        ErrorDomain::protocol, ErrorCode::invalid_argument,
        "URL must contain a host and no credentials, query, or fragment", false, 0, {"endpoint"}));
  while (!rest.empty() && rest.back() == '/')
    rest.pop_back();
  bool tls = scheme == "https" || scheme == "wss";
  return Result<std::string>::success(
      std::string(ws ? (tls ? "wss://" : "ws://") : (tls ? "https://" : "http://")) + rest);
}
}  // namespace livekit::sdk
