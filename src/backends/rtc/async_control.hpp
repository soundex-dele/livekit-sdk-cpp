#pragma once
#include "backends/rtc/control_context.hpp"
#include "livekit/rtc/observer.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <exception>
#include <memory>
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
  // A rejected/dropped notification must release its reservation as surely as
  // a delivered one. Construct before admission; arm only after enqueue succeeds.
  struct CompletionLease {
    AsyncControl* owner = nullptr;
    std::string key;
    bool barrier = false;
    bool started = false;
    std::atomic<bool> finished{false};

    ~CompletionLease() {
      finish();
    }

    void begin() {
      std::lock_guard lock(owner->mutex_);
      owner->keys_.erase(key);
      if (barrier) {
        owner->barrier_pending_ = false;
      }
      started = true;
      owner->changed_.notify_all();
    }

    void finish() {
      if (!owner || finished.exchange(true)) {
        return;
      }
      std::lock_guard lock(owner->mutex_);
      if (!started) {
        owner->keys_.erase(key);
        if (barrier) {
          owner->barrier_pending_ = false;
        }
      }
      --owner->outstanding_;
      owner->changed_.notify_all();
    }
  };

 public:
  using Task = std::function<void()>;
  using Failure = std::function<void(std::exception_ptr)>;

  AsyncControl(SessionControl& control, std::function<void(Task)> dispatch, Task wake,
               Failure failure = {})
      : control_(control),
        dispatch_(std::move(dispatch)),
        wake_(std::move(wake)),
        failure_(std::move(failure)),
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
    auto lease = std::make_shared<CompletionLease>();
    lease->key = key;
    lease->barrier = barrier;
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
                 completion = std::move(completion), lease](bool cancelled) mutable {
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
      dispatch_([observer, completion = std::move(completion), result = std::move(result),
                 lease]() mutable {
        // Release the operation identity before calling external code: observers
        // may submit the next operation of the same kind from this notification.
        lease->begin();
        try {
          if (auto target = observer.lock()) {
            completion(*target, std::move(result));
          }
        } catch (...) {
          lease->finish();
          throw;
        }
        lease->finish();
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
    lease->owner = this;
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

  // One coalesced internal task, independent of observer capacity. Requests
  // arriving during execution schedule another pass. Leave/close take priority.
  void set_maintenance(Task task) {
    auto handler = task ? std::make_shared<Task>(std::move(task)) : nullptr;
    std::lock_guard lock(mutex_);
    maintenance_ = std::move(handler);
  }

  void request_maintenance() {
    std::lock_guard lock(mutex_);
    if (!stopping_ && !terminal_) {
      maintenance_pending_ = true;
      changed_.notify_all();
    }
  }

  void shutdown() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      maintenance_pending_ = false;
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
    bool cancelled = false;
    bool barrier = false;
  };

  void run() {
    for (;;) {
      Work work;
      std::shared_ptr<Task> maintenance;
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] {
          return stopping_ || !queue_.empty() || (maintenance_pending_ && !barrier_pending_);
        });
        if (maintenance_pending_ && !barrier_pending_ && !stopping_) {
          maintenance_pending_ = false;
          maintenance = maintenance_;
        } else {
          if (queue_.empty()) {
            return;
          }
          work = std::move(queue_.front());
          queue_.pop_front();
          if (work.barrier && !stopping_) {
            control_.interrupted = false;
          }
        }
      }
      try {
        if (maintenance) {
          (*maintenance)();
        } else if (work.run) {
          work.run(work.cancelled);
        }
      } catch (...) {
        // Allocation/dispatch failures must not escape a std::thread entry.
        // Work destruction returns any undelivered completion reservation.
        if (failure_) {
          try {
            failure_(std::current_exception());
          } catch (...) {
          }
        }
      }
    }
  }

  SessionControl& control_;
  std::function<void(Task)> dispatch_;
  Task wake_;
  Failure failure_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Work> queue_;
  std::set<std::string> keys_;
  std::shared_ptr<Task> maintenance_;
  bool maintenance_pending_ = false;
  size_t outstanding_ = 0;
  bool stopping_ = false, terminal_ = false, barrier_pending_ = false;
  std::thread worker_;
};
}  // namespace livekit::sdk::rtc::detail
