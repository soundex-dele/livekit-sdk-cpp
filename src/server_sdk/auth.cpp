#include "server_sdk/auth.hpp"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <chrono>
#include <stdexcept>
namespace livekit::sdk {
static std::string sign_claims(const std::string& key, const std::string& secret, Json claims, int ttl) {
  if (key.empty() || secret.empty()) throw std::invalid_argument("API key and secret are required");
  if (ttl <= 0) throw std::invalid_argument("Token TTL must be positive");
  auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  claims["iss"] = key; claims["iat"] = now; claims["nbf"] = now; claims["exp"] = now + ttl;
  auto payload = base64url(R"({"alg":"HS256","typ":"JWT"})") + "." + base64url(claims.dump());
  unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int size = 0;
  if (!HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
            reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest, &size))
    throw std::runtime_error("HMAC-SHA256 failed");
  return payload + "." + base64url({reinterpret_cast<char*>(digest), size});
}
std::string make_token(const std::string& key, const std::string& secret,
                      const std::string& identity, const Json& grants, int ttl, const std::string& name) {
  return AccessToken(key, secret).set_identity(identity).set_video_grant(grants).set_valid_for(ttl).set_name(name).to_jwt();
}
AccessToken::AccessToken(std::string key, std::string secret) : key_(std::move(key)), secret_(std::move(secret)) {}
AccessToken& AccessToken::set_identity(std::string v) { claims_["sub"] = std::move(v); return *this; }
AccessToken& AccessToken::set_name(std::string v) { claims_["name"] = std::move(v); return *this; }
AccessToken& AccessToken::set_metadata(std::string v) { claims_["metadata"] = std::move(v); return *this; }
AccessToken& AccessToken::set_attributes(const std::map<std::string,std::string>& v) { claims_["attributes"] = v; return *this; }
AccessToken& AccessToken::set_kind(std::string v) { claims_["kind"] = std::move(v); return *this; }
AccessToken& AccessToken::set_valid_for(int v) { if (v <= 0) throw std::invalid_argument("Token TTL must be positive"); ttl_ = v; return *this; }
AccessToken& AccessToken::set_video_grant(Json v) { claims_["video"] = std::move(v); return *this; }
AccessToken& AccessToken::set_sip_grant(Json v) { claims_["sip"] = std::move(v); return *this; }
AccessToken& AccessToken::set_agent_grant(Json v) { claims_["agent"] = std::move(v); return *this; }
std::string AccessToken::to_jwt() const { return sign_claims(key_, secret_, claims_, ttl_); }
}
