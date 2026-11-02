#include <livekit/api.hpp>
#include <livekit/auth.hpp>
#include <iostream>
#include "../api_contract.hpp"

int main(int argc, char** argv) {
  if (argc == 2) {
    return api_contract(argv[1]);
  }
  using namespace livekit::sdk;
  // Must build and link without WebRTC, GraphFlow, FFmpeg or SDL.
  LiveKitAPI api({"http://127.0.0.1:1", {"consumer-key", "consumer-secret", ""}, false});
  auto token = api.room()
                   .create_token()
                   .set_identity("service-consumer")
                   .set_video_grant({{"roomJoin", true}, {"room", "consumer-room"}})
                   .to_jwt();
  if (!token) {
    std::cerr << token.error().message << '\n';
    return 1;
  }
  auto first = token->find('.');
  auto second = token->find('.', first + 1);
  if (first == std::string::npos || second == std::string::npos) {
    return 1;
  }
  auto decoded = base64_decode(token->substr(first + 1, second - first - 1));
  if (!decoded) {
    return 1;
  }
  auto claims = Json::parse(*decoded, nullptr, false);
  if (!claims.is_object() || claims["sub"] != "service-consumer" ||
      claims["video"]["room"] != "consumer-room") {
    return 1;
  }
  for (const char* invalid : {"A", "=", "====", "AA=A", "A===", "AB==", "!!"}) {
    auto value = base64_decode(invalid);
    if (value || value.error().code != ErrorCode::invalid_argument) {
      return 1;
    }
  }
  for (const char* valid : {"", "Zg", "Zg==", "Zm8", "Zm8=", "Zm9v"}) {
    if (!base64_decode(valid)) {
      return 1;
    }
  }
  if (endpoint("wss://user:secret@localhost", true) || make_token("", "", "test", Json::object()) ||
      AccessToken("key", "secret").set_valid_for(0).to_jwt() ||
      AccessToken("key", "secret").set_video_grant(1).to_jwt() ||
      AccessToken("key", "secret").set_name(std::string(1, char(0xff))).to_jwt()) {
    return 1;
  }
  if (api.room().get_participant(Json::object()) ||
      api.sip().create_sip_participant(Json{{"waitUntilAnswered", "yes"}}) ||
      api.sip().create_sip_participant(
          Json{{"waitUntilAnswered", true}, {"ringingTimeout", "NaNs"}})) {
    return 1;
  }
  LiveKitAPI invalid({"bad-url", {}, false});
  if (invalid.room().status() || invalid.room().list_rooms()) {
    return 1;
  }
  std::cout << "Server SDK explicit failure contracts passed\n";
}
