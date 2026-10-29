#pragma once
#include <chrono>
#include <functional>
#include <stop_token>
#include <string>

namespace livekit::sdk {
enum class TokenRequestReason { initial, authentication_failed };

struct TokenRequest {
  std::string url;
  TokenRequestReason reason = TokenRequestReason::initial;
  std::chrono::steady_clock::time_point deadline;
  std::stop_token stop;
};
enum class TokenFailure { none, unavailable, denied };

struct TokenResult {
  std::string token;
  TokenFailure failure = TokenFailure::none;
};

using TokenCompletion = std::function<void(TokenResult)>;
// Start an asynchronous operation and return promptly. Completion may run on
// any thread, including inline. The SDK accepts only the first completion and
// ignores completions after cancellation/deadline. The provider must own its
// async work, observe request.stop, and not call Room::wait()/disconnect() here.
// Never block this startup callback: arbitrary application code cannot be
// forcibly interrupted by the SDK. SDK-owned threads are not created for it.
using TokenProvider = std::function<void(TokenRequest, TokenCompletion)>;
}  // namespace livekit::sdk
