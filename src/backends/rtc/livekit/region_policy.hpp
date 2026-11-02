#pragma once
#include "livekit/compat/algorithm.hpp"
#include "livekit/reconnect.hpp"
#include "livekit/result.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <set>

namespace livekit::sdk::detail {
struct SignalEndpoint {
  std::string url, host, origin;
  bool secure = false, cloud = false;
};

inline Result<SignalEndpoint> signal_endpoint(const std::string& input) {
  const auto invalid = [] {
    return Result<SignalEndpoint>::failure(make_failure(
        ErrorDomain::signaling, ErrorCode::permission_denied, "Invalid signaling endpoint"));
  };
  if (input.size() > 4096 || input.find_first_of("?#@\\") != std::string::npos ||
      std::any_of(input.begin(), input.end(), [](unsigned char c) { return c <= 32 || c >= 127; }))
    return invalid();
  const auto split = input.find("://");
  if (split == std::string::npos)
    return invalid();
  auto scheme = input.substr(0, split);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (scheme != "ws" && scheme != "wss" && scheme != "http" && scheme != "https")
    return invalid();
  const bool secure = scheme == "wss" || scheme == "https";
  const auto slash = input.find('/', split + 3);
  auto authority = input.substr(split + 3, slash == std::string::npos ? slash : slash - split - 3);
  auto path = slash == std::string::npos ? std::string{} : input.substr(slash);
  while (!path.empty() && path.back() == '/')
    path.pop_back();
  std::transform(authority.begin(), authority.end(), authority.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (authority.empty())
    return invalid();
  std::string host, port_text;
  if (authority.front() == '[') {
    const auto end = authority.find(']');
    if (end == std::string::npos || end == 1)
      return invalid();
    host = authority.substr(0, end + 1);
    if (host.find_first_not_of("[]0123456789abcdef:.") != std::string::npos)
      return invalid();
    if (end + 1 < authority.size()) {
      if (authority[end + 1] != ':')
        return invalid();
      port_text = authority.substr(end + 2);
      if (port_text.empty())
        return invalid();
    }
  } else {
    const auto colon = authority.find(':');
    host = authority.substr(0, colon);
    if (host.empty() || host.front() == '.' || host.back() == '.' ||
        host.find("..") != std::string::npos ||
        host.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-.") != std::string::npos)
      return invalid();
    if (colon != std::string::npos) {
      port_text = authority.substr(colon + 1);
      if (port_text.empty())
        return invalid();
    }
  }
  int port = secure ? 443 : 80;
  if (!port_text.empty()) {
    const auto result =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (result.ec != std::errc{} || result.ptr != port_text.data() + port_text.size() ||
        port <= 0 || port > 65535)
      return invalid();
  }
  const auto origin = std::string(secure ? "wss://" : "ws://") + host +
                      (port == (secure ? 443 : 80) ? "" : ":" + std::to_string(port));
  const bool cloud = secure && port == 443 && path.empty() && host.size() > 14 &&
                     livekit::compat::ends_with(host, ".livekit.cloud");
  return Result<SignalEndpoint>::success({origin + path, host, origin, secure, cloud});
}

inline Result<std::string> trusted_destination(const std::string& original,
                                               const std::string& candidate,
                                               const RegionOptions& options) {
  const auto from = signal_endpoint(original), to = signal_endpoint(candidate);
  if (!from) {
    return Result<std::string>::failure(from.failure_record());
  }
  if (!to) {
    return Result<std::string>::failure(to.failure_record());
  }
  if (from->secure && !to->secure)
    return Result<std::string>::failure(make_failure(ErrorDomain::signaling,
                                                     ErrorCode::permission_denied,
                                                     "Signaling redirect would downgrade TLS"));
  if (from->url == to->url || (options.allow_cloud_regions && from->cloud && to->cloud))
    return Result<std::string>::success(to->url);
  for (const auto& trusted : options.trusted_endpoints) {
    auto endpoint = signal_endpoint(trusted);
    if (endpoint && endpoint->url == to->url) {
      return Result<std::string>::success(to->url);
    }
  }
  return Result<std::string>::failure(make_failure(ErrorDomain::signaling,
                                                   ErrorCode::permission_denied,
                                                   "Signaling destination is not trusted"));
}

struct RegionState {
  std::string active_url;
  std::vector<std::string> candidates;
  std::set<std::string> attempted;
  std::chrono::steady_clock::time_point expires{};

  void replace(const std::string& original, const RegionOptions& options,
               const std::vector<std::string>& urls) {
    std::vector<std::string> accepted;
    for (const auto& url : urls) {
      // A server list cannot expand the application's trust policy.
      auto trusted = trusted_destination(original, url, options);
      if (trusted && std::find(accepted.begin(), accepted.end(), *trusted) == accepted.end()) {
        accepted.push_back(std::move(*trusted));
      }
      if (accepted.size() == 32)
        break;
    }
    candidates = std::move(accepted);
    expires = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  }

  void select() {
    const auto active = signal_endpoint(active_url);
    const bool secure = !active || active->secure;
    for (const auto& candidate : candidates) {
      const auto parsed = signal_endpoint(candidate);
      if (parsed && !attempted.count(candidate) && (!secure || parsed->secure)) {
        active_url = candidate;
        break;
      }
    }
    attempted.insert(active_url);
  }
};
}  // namespace livekit::sdk::detail
