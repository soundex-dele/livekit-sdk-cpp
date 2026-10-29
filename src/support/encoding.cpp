#include "livekit/encoding.hpp"
#include <openssl/evp.h>
#include <stdexcept>

namespace livekit::sdk {
std::string base64url(std::string_view v) {
  std::string out(4 * ((v.size() + 2) / 3), '\0');
  if (!v.empty())
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                    reinterpret_cast<const unsigned char*>(v.data()), static_cast<int>(v.size()));
  for (auto& c : out) {
    if (c == '+')
      c = '-';
    if (c == '/')
      c = '_';
  }
  while (!out.empty() && out.back() == '=')
    out.pop_back();
  return out;
}

std::string base64_decode(std::string_view value) {
  std::string encoded(value);
  for (auto& c : encoded) {
    if (c == '-')
      c = '+';
    else if (c == '_')
      c = '/';
  }
  if (encoded.size() % 4 == 1)
    throw std::invalid_argument("Invalid Base64 length");
  while (encoded.size() % 4)
    encoded += '=';
  if (encoded.empty())
    return {};
  std::string decoded(encoded.size() / 4 * 3, '\0');
  auto length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
                                reinterpret_cast<const unsigned char*>(encoded.data()),
                                static_cast<int>(encoded.size()));
  if (length < 0)
    throw std::invalid_argument("Invalid Base64 data");
  if (encoded.back() == '=')
    --length;
  if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=')
    --length;
  decoded.resize(length);
  return decoded;
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

std::string endpoint(std::string url, bool ws) {
  auto pos = url.find("://");
  if (pos == std::string::npos)
    throw std::runtime_error("URL must start with http(s):// or ws(s)://");
  auto scheme = url.substr(0, pos);
  if (scheme != "http" && scheme != "https" && scheme != "ws" && scheme != "wss")
    throw std::runtime_error("Unsupported URL scheme");
  auto rest = url.substr(pos + 3);
  if (rest.empty() || rest.front() == '/' || rest.find_first_of("?#@\r\n") != std::string::npos)
    throw std::runtime_error("URL must contain a host and no credentials, query, or fragment");
  while (!rest.empty() && rest.back() == '/')
    rest.pop_back();
  bool tls = scheme == "https" || scheme == "wss";
  return std::string(ws ? (tls ? "wss://" : "ws://") : (tls ? "https://" : "http://")) + rest;
}
}  // namespace livekit::sdk
