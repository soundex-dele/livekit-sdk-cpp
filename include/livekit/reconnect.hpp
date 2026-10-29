#pragma once
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace livekit::sdk {
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

struct ReconnectOptions {
  bool enabled = true;
  size_t max_attempts = 6;
  std::chrono::milliseconds initial_delay{200};
  std::chrono::milliseconds max_delay{5000};
  std::chrono::milliseconds timeout{30000};
  // Resume and rejoin share max_attempts and timeout. Reserve attempts for a
  // fresh join when the original server session can no longer be resumed.
  size_t resume_attempts = 2;
};
enum class ReconnectMode { resume, rejoin };

struct ReconnectInfo {
  ReconnectMode mode = ReconnectMode::resume;
  size_t attempt = 0;
};
}  // namespace livekit::sdk
