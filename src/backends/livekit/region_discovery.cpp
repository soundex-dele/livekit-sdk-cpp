#include "backends/livekit/region_discovery.hpp"
#include "backends/livekit/region_policy.hpp"
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <mutex>

namespace livekit::sdk::client::detail {
std::vector<std::string> discover_regions(const ConnectionOptions& options, const Operation& op) {
  std::vector<std::string> result;
  auto endpoint = signal_endpoint(options.url);
  if (!endpoint || !endpoint->cloud || !endpoint->secure || !options.regions.discovery ||
      !op.status()) {
    return result;
  }
  static std::once_flag network;
  std::call_once(network, [] { ix::initNetSystem(); });
  ix::HttpClient client;
  auto request = client.createRequest();
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(op.deadline - SteadyClock::now());
  const auto timeout = std::min(remaining, options.regions.discovery_timeout);
  if (timeout.count() <= 0) {
    return result;
  }
  request->connectTimeout = request->transferTimeout = static_cast<int>(
      std::max<int64_t>(1, std::chrono::ceil<std::chrono::seconds>(timeout).count()));
  request->followRedirects = false;
  request->compress = false;
  request->extraHeaders["Authorization"] = "Bearer " + options.token;
  std::string body;
  request->onChunkCallback = [&](const std::string& chunk) {
    if (body.size() + chunk.size() > 65536 || !op.status()) {
      request->cancel = true;
      return;
    }
    body += chunk;
  };
  auto url = endpoint->origin;
  url.replace(0, 3, "https");  // validated wss origin
  auto response = client.get(url + "/settings/regions", request);
  if (!response || response->statusCode != 200 || request->cancel || !op.status()) {
    return result;
  }
  if (body.empty()) {
    body = response->body;
  }
  if (body.size() > 65536) {
    return result;
  }
  auto json = nlohmann::json::parse(body, nullptr, false);
  if (!json.is_object()) {
    return result;
  }
  const auto regions = json.find("regions");
  if (regions == json.end() || !regions->is_array()) {
    return result;
  }
  for (const auto& region : *regions) {
    if (!region.is_object()) {
      continue;
    }
    auto candidate = region.find("url");
    if (candidate == region.end() || !candidate->is_string()) {
      continue;
    }
    auto trusted = trusted_destination(options.url, candidate->get<std::string>(), options.regions);
    if (trusted) {
      result.push_back(std::move(*trusted));
    }
    if (result.size() == 32) {
      break;
    }
  }
  return result;
}
}  // namespace livekit::sdk::client::detail
