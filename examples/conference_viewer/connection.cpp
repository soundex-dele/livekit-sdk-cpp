#include "connection.hpp"
#include <livekit/auth.hpp>
#include <cstdlib>
#include <stdexcept>

namespace demo {
namespace {
std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
}
livekit::sdk::RoomOptions username_options(std::string url, std::string room, std::string username) {
  livekit::sdk::RoomOptions options;
  options.url = trim(std::move(url));
  options.room = trim(std::move(room));
  username = trim(std::move(username));
  if (options.url.empty() || options.room.empty() || username.empty())
    throw std::invalid_argument("Server URL, room and username are required.");
  const char* key = std::getenv("LIVEKIT_API_KEY");
  const char* secret = std::getenv("LIVEKIT_API_SECRET");
  const bool has_key = key && *key, has_secret = secret && *secret;
  if (has_key != has_secret)
    throw std::invalid_argument("Set both LIVEKIT_API_KEY and LIVEKIT_API_SECRET, or unset both for local testing.");
  options.token = livekit::sdk::AccessToken(has_key ? key : "uav-test",
      has_secret ? secret : "uav-local-test-secret-at-least-32-characters")
    .set_identity(username).set_name(username).set_valid_for(3600)
    .set_video_grant({{"roomJoin", true}, {"room", options.room},
                     {"canPublish", true}, {"canSubscribe", true}}).to_jwt();
  return options;
}
}
