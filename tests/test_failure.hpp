#pragma once
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace livekit::sdk::test {
[[noreturn]] inline void fail(std::string_view message) {
  std::cerr << "Test failed: " << message << '\n';
  std::exit(EXIT_FAILURE);
}
}  // namespace livekit::sdk::test
