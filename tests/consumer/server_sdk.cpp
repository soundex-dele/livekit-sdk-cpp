#include <livekit/api.hpp>
#include <livekit/auth.hpp>
#include <stdexcept>

int main() {
  // Must build and link without WebRTC, GraphFlow, FFmpeg or SDL.
  livekit::sdk::LiveKitAPI api(
      {"http://127.0.0.1:1", {"consumer-key", "consumer-secret", ""}, false});
  auto token = api.room()
                   .create_token()
                   .set_identity("service-consumer")
                   .set_video_grant({{"roomJoin", true}, {"room", "consumer-room"}})
                   .to_jwt();
  auto first = token.find('.');
  auto second = token.find('.', first + 1);
  if (first == std::string::npos || second == std::string::npos)
    throw std::runtime_error("JWT missing segments");
  auto claims = livekit::sdk::Json::parse(
      livekit::sdk::base64_decode(token.substr(first + 1, second - first - 1)));
  if (claims.at("sub") != "service-consumer" || claims.at("video").at("room") != "consumer-room")
    throw std::runtime_error("Incorrect service token claims");
}
