#include "livekit/compat/thread.hpp"
#include "support/process.hpp"
#include <cstdlib>
#include <iostream>
#include <future>

using namespace livekit::sdk;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--echo") {
    std::cout << argv[2];
    return 0;
  }
  lk::Process empty({});
  check(!empty.status() && !empty.wait(), "Invalid process must report failure");
  auto invalid = lk::Process::capture({argv[0], std::string("a\0b", 3)});
  check(!invalid && invalid.error().code == ErrorCode::invalid_argument, "NUL argument accepted");
  const std::string text = "two words \"quote\" trailing\\ $() & ;";
  auto output = lk::Process::capture({argv[0], "--echo", text});
  check(output && *output == text, "Process argument changed or capture failed");
  auto missing = lk::Process::capture({"__livekit_missing_process_for_test__"});
  check(!missing, "Missing executable did not report failure");
  std::cout << "Support result contracts passed\n";
}
