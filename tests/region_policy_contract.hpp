#pragma once
#include "backends/rtc/livekit/region_policy.hpp"

inline void region_policy_contract() {
  using namespace livekit::sdk;
  auto require = [](bool ok) {
    if (!ok)
      throw std::runtime_error("Region policy contract failed");
  };
  auto rejects = [&](auto fn) {
    try {
      fn();
    } catch (const SDKError&) {
      return;
    }
    throw std::runtime_error("Untrusted destination accepted");
  };
  RegionOptions policy;
  auto destination = [&](const std::string& url) {
    return detail::trusted_destination("wss://project.livekit.cloud", url, policy);
  };
  require(destination("https://PROJECT.livekit.cloud:443/") == "wss://project.livekit.cloud");
  require(destination("wss://project-us.livekit.cloud") == "wss://project-us.livekit.cloud");
  for (const auto& url :
       {"wss://project.livekit.cloud.evil.test", "wss://notlivekit.cloud",
        "ws://project.livekit.cloud", "wss://user@project.livekit.cloud",
        "wss://project.livekit.cloud?token=secret", "wss://project.livekit.cloud:99999",
        "wss://project.livekit.cloud:443junk", "wss://project.livekit.cloud\\@evil.test",
        "wss://project.livekit.cloud./", "wss://project.livekit.cloud:444",
        "wss://project.livekit.cloud/path"})
    rejects([&] { destination(url); });
  policy.allow_cloud_regions = false;
  rejects([&] { destination("wss://project-us.livekit.cloud"); });
  policy.trusted_endpoints = {"https://rtc.example:8443/tenant"};
  require(destination("wss://rtc.example:8443/tenant/") == "wss://rtc.example:8443/tenant");
  rejects([&] { destination("wss://rtc.example:8443/other"); });
  policy.trusted_endpoints.push_back("ws://insecure.example");
  rejects([&] { destination("ws://insecure.example"); });
  require(detail::signal_endpoint("http://[::1]:7880").url == "ws://[::1]:7880");
  detail::RegionState state;
  state.active_url = "wss://project.livekit.cloud";
  state.replace(
      state.active_url, policy,
      {"wss://evil.example", "wss://rtc.example:8443/tenant", "https://rtc.example:8443/tenant"});
  require(state.candidates.size() == 1);
  state.select();
  require(state.active_url == "wss://rtc.example:8443/tenant" && state.attempted.size() == 1);
}
