#pragma once
#include <chrono>
#include <functional>
#include "livekit/result.hpp"
#include "livekit/compat/thread.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace livekit::sdk::client {
struct RegionOptions {
  bool discovery = true;  // HTTPS /settings/regions, only for *.livekit.cloud
  bool allow_cloud_regions = true;
  // Exact endpoint URLs (including path). https/wss and http/ws are equivalent.
  // TLS downgrade is never permitted, even for an explicitly listed endpoint.
  std::vector<std::string> trusted_endpoints;
  size_t max_redirects = 3;
  std::chrono::milliseconds discovery_timeout{2000};
  bool initial_failover = true;
  // Per-endpoint socket + JoinResponse budget, capped by connect_timeout.
  std::chrono::milliseconds endpoint_timeout{5000};
  // Optional ordered fallback candidates (also subject to trusted_endpoints /
  // cloud policy). The trust list alone never causes connection attempts.
  std::vector<std::string> fallback_endpoints;
};

enum class TokenRequestReason { initial, authentication_failed };

struct TokenRequest {
  std::string url;
  TokenRequestReason reason = TokenRequestReason::initial;
  std::chrono::steady_clock::time_point deadline;
  livekit::compat::stop_token stop;
};

// Synchronous provider on the connection worker. Honor stop/deadline, return
// promptly, and do not invoke Room controls. A blocked provider delays close.
using TokenProvider = std::function<Result<std::string>(const TokenRequest&)>;
}  // namespace livekit::sdk::client
