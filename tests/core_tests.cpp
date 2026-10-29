#include "server_sdk/auth.hpp"
#include "support/process.hpp"

#include "livekit.pb.h"
#include <iostream>
#include <stdexcept>
#include <thread>
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--echo") { lk::Json args = lk::Json::array(); for (int i = 2; i < argc; ++i) args.push_back(argv[i]); std::cout << args.dump(); return 0; }
  try {
    check(lk::base64url("") == "", "base64 empty");
    check(lk::base64url("foo") == "Zm9v", "base64 triple");
    check(lk::base64url("fo") == "Zm8", "base64 padding");
    check(lk::percent_encode("a /+&?") == "a%20%2F%2B%26%3F", "URL encoding");
    check(lk::endpoint("https://localhost:7880/prefix/", true) == "wss://localhost:7880/prefix", "URL conversion");
    bool rejected = false; try { lk::endpoint("wss://user:secret@localhost", true); } catch (...) { rejected = true; } check(rejected, "URL credentials must be rejected");
    rejected = false; try { lk::make_token("", "", "test", lk::Json::object()); } catch (...) { rejected = true; } check(rejected, "empty API credentials rejected");
    livekit::SignalRequest request; request.mutable_ping_req()->set_timestamp(123);
    check(request.SerializeAsString() == std::string("\x82\x01\x02\x08\x7b", 5), "LiveKit ping protobuf field numbers");
    livekit::SignalResponse response;
    check(response.ParseFromString(std::string("\x0a\x04\x0a\x02\x12\x00", 6)), "minimal join wire decode"); check(response.has_join(), "join wire tag");
    std::vector<std::string> inputs{"", "two words", "quote\"test", "trailing\\", "back\\\"quote", "$() & ;"};
    std::vector<std::string> args{argv[0], "--echo"}; args.insert(args.end(), inputs.begin(), inputs.end());
    check(lk::Json::parse(lk::Process::capture(args)) == inputs, "child arguments must survive without shell interpretation");
    std::cout << "core tests passed\n"; return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
