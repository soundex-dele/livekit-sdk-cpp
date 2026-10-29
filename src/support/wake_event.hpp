#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <functional>
#include <utility>

namespace livekit::sdk::detail {
// Shared by native callbacks and their owner. Carries no owner pointer. A
// revision captured before processing work prevents wakeups between drain/wait
// from being lost. No mutex is held while the owner processes that work.
class WakeEvent {
 public:
  // A derived wake source can mark its registered work item before waking the
  // owner. The immutable relay must not borrow an owner or call application code.
  explicit WakeEvent(std::function<void()> relay = {}) : relay_(std::move(relay)) {}

  uint64_t revision() const {
    std::lock_guard lock(mutex_);
    return revision_;
  }

  void signal() {
    {
      std::lock_guard lock(mutex_);
      ++revision_;
      changed_.notify_all();
    }
    if (relay_) {
      relay_();
    }
  }

  void wait(uint64_t observed, std::stop_token stop,
            std::optional<std::chrono::steady_clock::time_point> deadline = {}) {
    std::unique_lock lock(mutex_);
    const auto changed = [&] { return revision_ != observed; };
    if (deadline) {
      changed_.wait_until(lock, stop, *deadline, changed);
    } else {
      changed_.wait(lock, stop, changed);
    }
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  uint64_t revision_ = 0;
  const std::function<void()> relay_;
};
}  // namespace livekit::sdk::detail
