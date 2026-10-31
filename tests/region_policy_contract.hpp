#pragma once
#include <cstdlib>
#include "backends/rtc/livekit/region_policy.hpp"

inline void region_policy_contract() {
  using namespace livekit::sdk;
  auto require = [](bool ok) {
    if (!ok)
      std::abort();
  };
  auto rejects = [&](auto fn) {
    auto result = fn();
    require(!result && result.error().code == ErrorCode::permission_denied);
  };
  auto accepted = [&](Result<std::string> result, const std::string& expected) {
    require(result && *result == expected);
  };
  RegionOptions policy;
  auto destination = [&](const std::string& url) {
    return detail::trusted_destination("wss://project.livekit.cloud", url, policy);
  };
  accepted(destination("https://PROJECT.livekit.cloud:443/"), "wss://project.livekit.cloud");
  accepted(destination("wss://project-us.livekit.cloud"), "wss://project-us.livekit.cloud");
  for (const auto& url :
       {"wss://project.livekit.cloud.evil.test", "wss://notlivekit.cloud",
        "ws://project.livekit.cloud", "wss://user@project.livekit.cloud",
        "wss://project.livekit.cloud?token=secret", "wss://project.livekit.cloud:99999",
        "wss://project.livekit.cloud:443junk", "wss://project.livekit.cloud\\@evil.test",
        "wss://project.livekit.cloud./", "wss://project.livekit.cloud:444",
        "wss://project.livekit.cloud/path"})
    rejects([&] { return destination(url); });
  policy.allow_cloud_regions = false;
  rejects([&] { return destination("wss://project-us.livekit.cloud"); });
  policy.trusted_endpoints = {"https://rtc.example:8443/tenant"};
  accepted(destination("wss://rtc.example:8443/tenant/"), "wss://rtc.example:8443/tenant");
  rejects([&] { return destination("wss://rtc.example:8443/other"); });
  policy.trusted_endpoints.push_back("ws://insecure.example");
  rejects([&] { return destination("ws://insecure.example"); });
  require(detail::signal_endpoint("http://[::1]:7880")->url == "ws://[::1]:7880");
  detail::RegionState state;
  state.active_url = "wss://project.livekit.cloud";
  state.replace(
      state.active_url, policy,
      {"wss://evil.example", "wss://rtc.example:8443/tenant", "https://rtc.example:8443/tenant"});
  require(state.candidates.size() == 1);
  state.select();
  require(state.active_url == "wss://rtc.example:8443/tenant" && state.attempted.size() == 1);
}
