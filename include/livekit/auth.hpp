#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <map>

namespace livekit::sdk {
using Json = nlohmann::json;
std::string make_token(const std::string& key, const std::string& secret,
                       const std::string& identity, const Json& grants,
                       int ttl_seconds = 3600, const std::string& name = "");
std::string base64url(std::string_view value);
std::string percent_encode(std::string_view value);
std::string endpoint(std::string url, bool websocket);
std::string base64_decode(std::string_view value);
// Go auth.AccessToken equivalent. Grants are protobuf-JSON objects.
class AccessToken {
 public:
  AccessToken(std::string key, std::string secret);
  AccessToken& set_identity(std::string identity);
  AccessToken& set_name(std::string name);
  AccessToken& set_metadata(std::string metadata);
  AccessToken& set_attributes(const std::map<std::string, std::string>& attributes);
  AccessToken& set_kind(std::string kind);
  AccessToken& set_valid_for(int seconds);
  AccessToken& set_video_grant(Json grant);
  AccessToken& set_sip_grant(Json grant);
  AccessToken& set_agent_grant(Json grant);
  std::string to_jwt() const;
 private:
  std::string key_, secret_;
  Json claims_ = Json::object();
  int ttl_ = 3600;
};
}
