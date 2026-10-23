#pragma once
#include "livekit/auth.hpp"
#include <memory>
#include <stop_token>
#include <stdexcept>

namespace livekit::sdk {
struct Credentials { std::string api_key, api_secret, token; };
struct RequestOptions {
  int connect_timeout = 10;
  int timeout = 30;
  std::stop_token stop;
};
class APIError : public std::runtime_error {
 public:
  APIError(std::string message, int status, std::string code, Json metadata = Json::object());
  int status() const noexcept { return status_; }
  const std::string& code() const noexcept { return code_; }
  const Json& metadata() const noexcept { return metadata_; }
 private:
  int status_;
  std::string code_;
  Json metadata_;
};
namespace detail { class APITransport; }
class ServiceClient {
 public:
  // Explicit construction does not read environment variables.
  ServiceClient(std::string url, Credentials credentials);
  AccessToken create_token() const;
 protected:
  Json request(const std::string& service, const std::string& method, Json body,
               const Json& grants, const RequestOptions& options) const;
  std::shared_ptr<detail::APITransport> transport_;
};
}
