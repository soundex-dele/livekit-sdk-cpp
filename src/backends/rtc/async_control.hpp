#pragma once
#include "backends/rtc/control_context.hpp"
#include "livekit/rtc/observer.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <set>
#include <thread>

namespace livekit::sdk::rtc::detail {
template <class Observer>
class ObserverSlot {
 public:
  void set(std::weak_ptr<Observer> value) {
    std::lock_guard lock(mutex_);
    observer_ = std::move(value);
  }

  std::weak_ptr<Observer> get() const {
    std::lock_guard lock(mutex_);
    return observer_;
  }

 private:
  mutable std::mutex mutex_;
  std::weak_ptr<Observer> observer_;
};

inline OperationResult operation_result(ErrorCode code, const std::string& operation) {
  if (code == ErrorCode::ok) {
    return {};
  }
  auto failure = control_failure;
  if (!failure || failure->code != code) {
    failure = std::make_shared<FailureInfo>(FailureInfo{next_failure_id(),
                                                        ErrorDomain::lifecycle,
                                                        code,
                                                        operation + " failed",
                                                        false,
                                                        0,
                                                        {operation}});
  }
  return {code, std::move(failure)};
}

// One bounded command queue per room, shared by RoomEngine and RtcEngine.
// The notification dispatcher must retain every accepted completion. Capacity
// remains reserved until delivery, so a slow observer cannot grow the queue.
class AsyncControl {
 public:
  using Task = std::function<void()>;

  AsyncControl(SessionControl& control, std::function<void(Task)> dispatch, Task wake)
      : control_(control),
        dispatch_(std::move(dispatch)),
        wake_(std::move(wake)),
        worker_([this] { run(); }) {}

  ~AsyncControl() {
    shutdown();
  }

  template <class Function, class Observer, class Completion>
  ErrorCode submit(std::string key, Function function, std::weak_ptr<Observer> observer,
                   Completion completion, bool barrier = false, bool terminal = false) {
    using Value = std::invoke_result_t<Function>;
    if (observer.expired()) {
      return ErrorCode::invalid_state;
    }
    std::lock_guard lock(mutex_);
    if (stopping_ || terminal_) {
      return ErrorCode::session_closed;
    }
    if (barrier_pending_ || keys_.contains(key)) {
      return ErrorCode::busy;
    }
    // Reserve one extra slot for leave/close even when normal capacity is full.
    if (outstanding_ >= (barrier ? 129u : 128u)) {
      return ErrorCode::resource_exhausted;
    }
    auto work = [this, key, function = std::move(function), observer,
                 completion = std::move(completion), barrier](bool cancelled) mutable {
      control_failure.reset();
      auto value = control_boundary([&]() -> Value {
        if (cancelled) {
          if constexpr (std::is_same_v<Value, ErrorCode>) {
            return ErrorCode::cancelled;
          } else {
            return Value::failure(std::make_shared<FailureInfo>(
                FailureInfo{next_failure_id(),
                            ErrorDomain::lifecycle,
                            ErrorCode::cancelled,
                            "RTC operation cancelled before execution",
                            false,
                            0,
                            {key}}));
          }
        }
        return function();
      });
      auto result = [&] {
        if constexpr (std::is_same_v<Value, ErrorCode>) {
          return operation_result(value, key);
        } else {
          return std::move(value);
        }
      }();
      dispatch_([this, key, observer, completion = std::move(completion),
                 result = std::move(result), barrier]() mutable {
        // Release the operation identity before calling external code: observers
        // may submit the next operation of the same kind from this notification.
        {
          std::lock_guard lock(mutex_);
          keys_.erase(key);
          if (barrier) {
            barrier_pending_ = false;
          }
        }
        try {
          if (auto target = observer.lock()) {
            completion(*target, std::move(result));
          }
        } catch (...) {
          finish();
          throw;
        }
        finish();
      });
    };
    keys_.insert(key);
    try {
      queue_.push_back({std::move(work), false, barrier});
    } catch (...) {
      keys_.erase(key);
      throw;
    }
    ++outstanding_;
    if (barrier) {
      barrier_pending_ = true;
      terminal_ = terminal;
      for (auto& item : queue_) {
        if (!item.barrier) {
          item.cancelled = true;
        }
      }
      control_.interrupted = true;
      wake_();
    }
    changed_.notify_all();
    return ErrorCode::ok;
  }

  void shutdown() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      for (auto& item : queue_) {
        item.cancelled = true;
      }
      control_.interrupted = true;
      wake_();
      changed_.notify_all();
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return outstanding_ == 0; });
  }

 private:
  struct Work {
    std::function<void(bool)> run;
    bool cancelled;
    bool barrier;
  };

  void finish() {
    std::lock_guard lock(mutex_);
    --outstanding_;
    changed_.notify_all();
  }

  void run() {
    for (;;) {
      Work work;
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) {
          return;
        }
        work = std::move(queue_.front());
        queue_.pop_front();
        if (work.barrier) {
          control_.interrupted = false;
        }
      }
      work.run(work.cancelled);
    }
  }

  SessionControl& control_;
  std::function<void(Task)> dispatch_;
  Task wake_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Work> queue_;
  std::set<std::string> keys_;
  size_t outstanding_ = 0;
  bool stopping_ = false, terminal_ = false, barrier_pending_ = false;
  std::thread worker_;
};
}  // namespace livekit::sdk::rtc::detail
