#include "livekit/compat/thread.hpp"
#include "livekit/api.hpp"
#include "server_sdk/api_transport.hpp"
#include "server_sdk/json_validation.hpp"
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <openssl/rand.h>
#include <cstdlib>

namespace livekit::sdk {
namespace detail {
Result<std::string> random_base64(int bytes) {
  if (bytes <= 0) {
    return Result<std::string>::failure(make_failure(
        ErrorDomain::protocol, ErrorCode::invalid_argument, "Random byte count must be positive"));
  }
  std::string value(static_cast<size_t>(bytes), '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(value.data()), bytes) != 1) {
    return Result<std::string>::failure(make_failure(
        ErrorDomain::protocol, ErrorCode::processing_failed, "Random generation failed"));
  }
  auto encoded = base64url(value);
  for (auto& c : encoded) {
    if (c == '-') {
      c = '+';
    } else if (c == '_') {
      c = '/';
    }
  }
  while (encoded.size() % 4) {
    encoded += '=';
  }
  return Result<std::string>::success(std::move(encoded));
}

Failure api_failure(std::string message, int status, std::string code,
                    const Json& metadata = Json::object()) {
  ErrorCode mapped = ErrorCode::request_failed;
  if (code == "canceled") {
    mapped = ErrorCode::cancelled;
  } else if (code == "deadline_exceeded") {
    mapped = ErrorCode::timeout;
  } else if (code == "permission_denied" || code == "unauthenticated") {
    mapped = ErrorCode::permission_denied;
  } else if (code == "unavailable") {
    mapped = ErrorCode::connection_lost;
  } else if (code == "malformed") {
    mapped = ErrorCode::malformed_message;
  }
  auto failure = std::make_shared<FailureInfo>();
  failure->id = next_failure_id();
  failure->domain = ErrorDomain::protocol;
  failure->code = mapped;
  failure->message = std::move(message);
  failure->backend_reason = status;
  failure->backend_code = std::move(code);
  failure->backend_metadata = metadata.dump();
  return failure;
}

class APITransport {
 public:
  std::string url;
  Credentials credentials;

  Failure initialization_failure;

  APITransport(std::string u, Credentials c) : credentials(std::move(c)) {
    auto normalized = endpoint(std::move(u), false);
    if (!normalized) {
      initialization_failure = normalized.failure_record();
    } else {
      url = std::move(*normalized);
    }
    if (!initialization_failure && credentials.token.empty() &&
        (credentials.api_key.empty() || credentials.api_secret.empty())) {
      initialization_failure = make_failure(ErrorDomain::protocol, ErrorCode::invalid_argument,
                                            "Token or API key and secret are required");
    }
  }

  Result<Json> call(const std::string& service, const std::string& method, const Json& body,
                    const Json& grants, const RequestOptions& options) const {
    const FailureContext context{service + "." + method};
    auto fail = [&](Failure failure) {
      return Result<Json>::failure(with_context(std::move(failure), context));
    };
    if (initialization_failure) {
      return fail(initialization_failure);
    }
    if (options.connect_timeout <= 0 || options.timeout <= 0) {
      return fail(make_failure(ErrorDomain::protocol, ErrorCode::invalid_argument,
                               "Request timeouts must be positive"));
    }
    if (!body.is_object() || !grants.is_object()) {
      return fail(make_failure(ErrorDomain::protocol, ErrorCode::invalid_argument,
                               "Request body and grants must be JSON objects"));
    }
    auto serialized = serialize_json(body);
    if (!serialized) {
      return fail(serialized.failure_record());
    }
    if (options.stop.stop_requested()) {
      return fail(api_failure("Request cancelled", 0, "canceled"));
    }
    static const bool ready = ix::initNetSystem();
    if (!ready)
      return fail(api_failure("Network initialization failed", 0, "unavailable"));
    auto token = credentials.token;
    if (token.empty()) {
      auto access = AccessToken(credentials.api_key, credentials.api_secret).set_valid_for(600);
      if (grants.contains("video"))
        access.set_video_grant(grants.at("video"));
      if (grants.contains("sip"))
        access.set_sip_grant(grants.at("sip"));
      if (grants.contains("agent"))
        access.set_agent_grant(grants.at("agent"));
      auto signed_token = access.to_jwt();
      if (!signed_token) {
        return fail(signed_token.failure_record());
      }
      token = std::move(*signed_token);
    }
    ix::HttpClient client;
    auto args = client.createRequest();
    args->connectTimeout = options.connect_timeout;
    args->transferTimeout = options.timeout;
    args->followRedirects = false;
    args->compress = false;
    // Unique per logical call. No automatic mutation retries are performed.
    std::string raw(16, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(raw.data()), static_cast<int>(raw.size())) !=
        1) {
      return fail(make_failure(ErrorDomain::protocol, ErrorCode::processing_failed,
                               "Request ID generation failed"));
    }
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
    livekit::compat::stop_callback cancel(options.stop, [&] { args->cancel = true; });
    auto response =
        client.post(url + "/twirp/livekit." + service + "/" + method, *serialized, args);
    if (options.stop.stop_requested())
      return fail(api_failure("Request cancelled", 0, "canceled"));
    if (response->errorCode != ix::HttpErrorCode::Ok)
      return fail(api_failure(response->errorMsg, 0, "unavailable"));
    auto result = Json::parse(response->body.empty() ? "{}" : response->body, nullptr, false);
    if (response->statusCode < 200 || response->statusCode >= 300) {
      if (!result.is_object()) {
        return fail(api_failure("API returned a non-JSON error", response->statusCode, "unknown"));
      }
      auto message = result.find("msg"), code = result.find("code"), meta = result.find("meta");
      if ((message != result.end() && !message->is_string()) ||
          (code != result.end() && !code->is_string())) {
        return fail(api_failure("API returned malformed error fields", response->statusCode,
                                "malformed", result));
      }
      return fail(api_failure(
          message == result.end() ? "API request failed" : message->get<std::string>(),
          response->statusCode, code == result.end() ? "unknown" : code->get<std::string>(),
          meta == result.end() ? Json::object() : *meta));
    }
    if (result.is_discarded()) {
      return fail(api_failure("API returned invalid JSON", response->statusCode, "malformed"));
    }
    return Result<Json>::success(std::move(result));
  }
};
}  // namespace detail

ServiceClient::ServiceClient(std::string url, Credentials credentials)
    : transport_(std::make_shared<detail::APITransport>(std::move(url), std::move(credentials))) {}

Result<void> ServiceClient::status() const {
  return transport_->initialization_failure
             ? Result<void>::failure(transport_->initialization_failure)
             : Result<void>::success();
}

AccessToken ServiceClient::create_token() const {
  return {transport_->credentials.api_key, transport_->credentials.api_secret};
}

Result<Json> ServiceClient::request(const std::string& service, const std::string& method,
                                    Json body, const Json& grants,
                                    const RequestOptions& options) const {
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
