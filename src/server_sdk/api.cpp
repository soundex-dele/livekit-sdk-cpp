#include "livekit/api.hpp"
#include "server_sdk/api_transport.hpp"
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <openssl/rand.h>
#include <cstdlib>

namespace livekit::sdk {
APIError::APIError(std::string message, int status, std::string code, Json metadata)
    : std::runtime_error(std::move(message)),
      status_(status),
      code_(std::move(code)),
      metadata_(std::move(metadata)) {}

namespace detail {
std::string random_base64(int bytes) {
  std::string value(bytes, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(value.data()), bytes) != 1)
    throw std::runtime_error("Random generation failed");
  auto encoded = base64url(value);
  for (auto& c : encoded) {
    if (c == '-')
      c = '+';
    else if (c == '_')
      c = '/';
  }
  while (encoded.size() % 4)
    encoded += '=';
  return encoded;
}

class APITransport {
 public:
  std::string url;
  Credentials credentials;

  APITransport(std::string u, Credentials c)
      : url(endpoint(std::move(u), false)), credentials(std::move(c)) {
    if (credentials.token.empty() &&
        (credentials.api_key.empty() || credentials.api_secret.empty()))
      throw std::invalid_argument("Token or API key and secret are required");
  }

  Json call(const std::string& service, const std::string& method, const Json& body,
            const Json& grants, const RequestOptions& options) const {
    if (options.connect_timeout <= 0 || options.timeout <= 0)
      throw std::invalid_argument("Request timeouts must be positive");
    if (options.stop.stop_requested())
      throw APIError("Request cancelled", 0, "canceled");
    static const bool ready = ix::initNetSystem();
    if (!ready)
      throw APIError("Network initialization failed", 0, "unavailable");
    auto token = credentials.token;
    if (token.empty()) {
      auto access = AccessToken(credentials.api_key, credentials.api_secret).set_valid_for(600);
      if (grants.contains("video"))
        access.set_video_grant(grants.at("video"));
      if (grants.contains("sip"))
        access.set_sip_grant(grants.at("sip"));
      if (grants.contains("agent"))
        access.set_agent_grant(grants.at("agent"));
      token = access.to_jwt();
    }
    ix::HttpClient client;
    auto args = client.createRequest();
    args->connectTimeout = options.connect_timeout;
    args->transferTimeout = options.timeout;
    args->followRedirects = false;
    args->compress = false;
    // Unique per logical call. No automatic mutation retries are performed.
    auto raw = base64_decode(random_base64(16));
    raw[6] = static_cast<char>((raw[6] & 15) | 64);
    raw[8] = static_cast<char>((raw[8] & 63) | 128);
    const char* hex = "0123456789abcdef";
    std::string request_id;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10)
        request_id += '-';
      auto c = static_cast<unsigned char>(raw[i]);
      request_id += hex[c >> 4];
      request_id += hex[c & 15];
    }
    args->extraHeaders = {{"Authorization", "Bearer " + token},
                          {"Content-Type", "application/json"},
                          {"X-Livekit-Request-Id", request_id}};
    std::stop_callback cancel(options.stop, [&] { args->cancel = true; });
    auto response =
        client.post(url + "/twirp/livekit." + service + "/" + method, body.dump(), args);
    if (options.stop.stop_requested())
      throw APIError("Request cancelled", 0, "canceled");
    if (response->errorCode != ix::HttpErrorCode::Ok)
      throw APIError(response->errorMsg, 0, "unavailable");
    auto result = Json::parse(response->body.empty() ? "{}" : response->body, nullptr, false);
    if (response->statusCode < 200 || response->statusCode >= 300) {
      const bool object = result.is_object();
      throw APIError(
          object ? result.value("msg", "API request failed") : "API returned a non-JSON error",
          response->statusCode, object ? result.value("code", "unknown") : "unknown",
          object ? result.value("meta", Json::object()) : Json::object());
    }
    if (result.is_discarded())
      throw APIError("API returned invalid JSON", response->statusCode, "malformed");
    return result;
  }
};
}  // namespace detail

ServiceClient::ServiceClient(std::string url, Credentials credentials)
    : transport_(std::make_shared<detail::APITransport>(std::move(url), std::move(credentials))) {}

AccessToken ServiceClient::create_token() const {
  return {transport_->credentials.api_key, transport_->credentials.api_secret};
}

Json ServiceClient::request(const std::string& service, const std::string& method, Json body,
                            const Json& grants, const RequestOptions& options) const {
  return transport_->call(service, method, body, grants, options);
}

namespace {
std::string environment(const char* name) {
  auto value = std::getenv(name);
  return value ? value : "";
}

APIOptions resolve(APIOptions o) {
  if (o.use_environment) {
    if (o.url.empty())
      o.url = environment("LIVEKIT_URL");
    auto& c = o.credentials;
    if (c.token.empty() && c.api_key.empty() && c.api_secret.empty())
      c.token = environment("LIVEKIT_TOKEN");
    if (c.token.empty() && c.api_key.empty() && c.api_secret.empty()) {
      c.api_key = environment("LIVEKIT_API_KEY");
      c.api_secret = environment("LIVEKIT_API_SECRET");
    }
  }
  return o;
}
}  // namespace

LiveKitAPI::LiveKitAPI(APIOptions options) : LiveKitAPI(resolve(std::move(options)), 0) {}

LiveKitAPI::LiveKitAPI(APIOptions o, int)
    : room_(o.url, o.credentials),
      egress_(o.url, o.credentials),
      ingress_(o.url, o.credentials),
      sip_(o.url, o.credentials),
      agent_dispatch_(o.url, o.credentials),
      connector_(o.url, o.credentials) {}
}  // namespace livekit::sdk
