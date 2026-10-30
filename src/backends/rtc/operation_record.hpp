#pragma once

#include "livekit/result.hpp"
#include <condition_variable>
#include <mutex>
#include <optional>

namespace livekit::sdk::rtc::detail {
// Privately shared by a calling thread and the existing room event loop. The
// event loop owns deadlines/epochs; completion arbitration is exactly once.
template <class T>
class OperationRecord {
 public:
  template <class... Args>
  void succeed(Args&&... args) {
    finish(Result<T>::success(std::forward<Args>(args)...));
  }

  void fail(std::exception_ptr error) {
    finish(Result<T>::failure(describe_failure(error)));
  }

  Result<T> await_completion() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return result_.has_value(); });
    return std::move(*result_);
  }

 private:
  void finish(Result<T> result) {
    {
      std::lock_guard lock(mutex_);
      if (result_) {
        return;
      }
      result_.emplace(std::move(result));
    }
    changed_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<Result<T>> result_;
};
}  // namespace livekit::sdk::rtc::detail
