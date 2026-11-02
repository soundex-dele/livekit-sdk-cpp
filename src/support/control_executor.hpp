#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include "livekit/result.hpp"

namespace livekit::sdk::detail {
// One bounded, ordered control callback lane. Media/RPC workers stay independent.
class ControlExecutor {
 public:
  explicit ControlExecutor(size_t capacity, std::function<void()> failed)
      : capacity_(capacity), failed_(std::move(failed)), thread_([this] { run(); }) {}

  ~ControlExecutor() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      ready_.notify_all();
    }
    if (thread_.joinable())
      thread_.join();
  }

  bool current() const {
    return thread_.get_id() == std::this_thread::get_id();
  }

  Result<void> post(std::function<Result<void>()> fn) {
    std::lock_guard lock(mutex_);
    if (error_) {
      return Result<void>::failure(error_);
    }
    if (stopping_ || queue_.size() >= capacity_) {
      return Result<void>::failure(
          make_failure(ErrorDomain::lifecycle,
                       stopping_ ? ErrorCode::session_closed : ErrorCode::resource_exhausted,
                       "Control callback queue is closed or exhausted"));
    }
    if (!fn) {
      return Result<void>::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_argument,
                                                "Empty control callback"));
    }
    queue_.push_back(std::move(fn));
    ready_.notify_one();
    return Result<void>::success();
  }

  Result<void> drain() {
    std::unique_lock lock(mutex_);
    if (current()) {
      return Result<void>::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_thread,
                                                "Cannot drain the control callback thread"));
    }
    ready_.wait(lock, [&] { return queue_.empty() && !active_; });
    if (error_) {
      return Result<void>::failure(error_);
    }
    return Result<void>::success();
  }

 private:
  void run() {
    for (;;) {
      std::function<Result<void>()> fn;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
        if (queue_.empty() && stopping_)
          return;
        fn = std::move(queue_.front());
        queue_.pop_front();
        active_ = true;
      }
      if (auto result = fn(); !result) {
        std::deque<std::function<Result<void>()>> abandoned;
        {
          std::lock_guard lock(mutex_);
          if (!error_)
            error_ = result.failure_record();
          abandoned.swap(queue_);
        }
        if (failed_) {
          failed_();
        }
      }
      {
        std::lock_guard lock(mutex_);
        active_ = false;
        ready_.notify_all();
      }
    }
  }

  size_t capacity_;
  std::function<void()> failed_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<Result<void>()>> queue_;
  Failure error_;
  bool active_ = false, stopping_ = false;
  std::thread thread_;
};
}  // namespace livekit::sdk::detail
