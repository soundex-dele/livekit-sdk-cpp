#pragma once
#include "livekit/participant.hpp"
#include "livekit/error.hpp"
#include <atomic>
#include <functional>

namespace livekit::sdk::detail {
struct Command : std::enable_shared_from_this<Command> {
  std::string operation;
  Json args;
  MediaSpec media;
  TrackPublishOptions publication;
  OperationOptions options;
  std::chrono::steady_clock::time_point deadline;
  std::function<void(const Json&, std::exception_ptr)> completion;
  // Completion and cancellation can race. Only the winning state transition
  // invokes the immutable completion function, outside all Room/native locks.
  bool begin_send() { auto expected = Phase::queued; return phase_.compare_exchange_strong(expected, Phase::sent); }
  bool done() const { return phase_ == Phase::finished; }
  bool resolve(const Json& result = Json::object()) {
    if (phase_.exchange(Phase::finished) == Phase::finished) return false;
    completion(result, {}); return true;
  }
  void reject(std::exception_ptr error) {
    if (phase_.exchange(Phase::finished) != Phase::finished) completion({}, error);
  }
  void interrupt(ErrorCode reason, const char* message) {
    const auto prior = phase_.exchange(Phase::finished);
    if (prior == Phase::finished) return;
    const auto code = prior == Phase::sent ? ErrorCode::outcome_unknown : reason;
    completion({}, std::make_exception_ptr(SDKError(ErrorDomain::lifecycle, code, message)));
  }
  void expire(std::chrono::steady_clock::time_point now) {
    if (now >= deadline) interrupt(ErrorCode::timeout, "Operation deadline exceeded; sent work may already have taken effect");
  }
  void arm_cancellation() {
    if (options.stop.stop_possible()) cancellation_ = std::make_unique<StopCallback>(options.stop, [weak = weak_from_this()] {
      if (auto command = weak.lock()) command->interrupt(ErrorCode::cancelled,
          "Operation cancelled; sent work may already have taken effect");
    });
  }
 private:
  enum class Phase { queued, sent, finished };
  std::atomic<Phase> phase_{Phase::queued};
  using StopCallback = std::stop_callback<std::function<void()>>;
  std::unique_ptr<StopCallback> cancellation_;
};
}
