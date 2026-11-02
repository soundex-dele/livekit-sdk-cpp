#pragma once
#include "livekit/compat/thread.hpp"
#include "livekit/auth.hpp"
#include <memory>

namespace livekit::sdk {
struct Credentials {
  std::string api_key, api_secret, token;
};

struct RequestOptions {
  int connect_timeout = 10;
  int timeout = 30;
  livekit::compat::stop_token stop;
};

namespace detail {
class APITransport;
}

class ServiceClient {
 public:
  // Explicit construction does not read environment variables.
  ServiceClient(std::string url, Credentials credentials);
  AccessToken create_token() const;
  // Configuration failures are also returned by every request.
  Result<void> status() const;

 protected:
  Result<Json> request(const std::string& service, const std::string& method, Json body,
                       const Json& grants, const RequestOptions& options) const;
  std::shared_ptr<detail::APITransport> transport_;
};
}  // namespace livekit::sdk
