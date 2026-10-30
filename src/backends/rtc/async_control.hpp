#pragma once
#include "backends/rtc/control_context.hpp"
#include "livekit/rtc/completion.hpp"
#include "livekit/async_result.hpp"
#include <array>
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

inline Result<void> operation_result(ErrorCode code, const std::string& operation) {
  if (code == ErrorCode::ok) {
    return Result<void>::success();
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
  return Result<void>::failure(std::move(failure));
}

// One bounded command queue owned by each RtcEngine execution context.
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

  // Execution ends at protocol settlement, independently of observer delivery.
  // Each admitted operation has at most one ready continuation. The fixed slot
  // array bounds scheduling storage even when every command is awaiting an ACK.
  struct Execution {
    AsyncControl* owner = nullptr;
    std::shared_ptr<CompletionLease> completion;
    std::string lane;
    bool fence = false;
    size_t slot = 129;
    std::function<void()> ready;

    ~Execution() {
      finish();
    }

    void finish() {
      if (!owner) {
        return;
      }
      auto* queue = owner;
      std::lock_guard lock(queue->mutex_);
      if (slot != 129) {
        queue->active_[slot] = nullptr;
        slot = 129;
        --queue->active_count_;
        queue->changed_.notify_all();
      }
      // A settled AsyncResult may retain its callback until its source returns.
      // Its final destruction must not touch a queue that has already drained.
      owner = nullptr;
    }
  };

 public:
  using Task = std::function<void()>;
  using Failure = std::function<void(std::exception_ptr)>;
  using Maintenance = std::function<AsyncResult<ErrorCode>()>;

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

  template <class Function>
  using Completed = std::conditional_t<
      std::is_same_v<typename async_detail::Unwrap<std::invoke_result_t<Function>>::Type,
                     ErrorCode>,
      Result<void>, typename async_detail::Unwrap<std::invoke_result_t<Function>>::Type>;

  template <class Function>
  ErrorCode submit(std::string key, Function function,
                   std::function<void(Completed<Function>)> completion, bool barrier = false,
                   bool terminal = false) {
    using Return = std::invoke_result_t<Function>;
    using Value = typename async_detail::Unwrap<Return>::Type;
    if (!completion && !barrier) {
      return ErrorCode::invalid_argument;
    }
    auto lease = std::make_shared<CompletionLease>();
    lease->key = key;
    lease->barrier = barrier;
    auto execution = std::make_shared<Execution>();
    execution->completion = lease;
    execution->lane = lane_for(key);
    execution->fence = barrier || key == "room.join";
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
    auto deliver = [this, key, completion = std::move(completion), lease,
                    execution](Value value) mutable {
      auto result = [&] {
        if constexpr (std::is_same_v<Value, ErrorCode>) {
          return operation_result(value, key);
        } else {
          return std::move(value);
        }
      }();
      execution->finish();
      dispatch_([completion = std::move(completion), result = std::move(result), lease]() mutable {
        // Release the operation identity before calling external code: observers
        // may submit the next operation of the same kind from this notification.
        lease->begin();
        try {
          if (completion) {
            completion(std::move(result));
          }
        } catch (...) {
          lease->finish();
          throw;
        }
        lease->finish();
      });
    };
    auto work = [this, key, function = std::move(function), deliver = std::move(deliver),
                 execution](bool cancelled) mutable {
      control_failure.reset();
      if (cancelled) {
        deliver(control_boundary([&]() -> Value {
          throw SDKError(ErrorDomain::lifecycle, ErrorCode::cancelled,
                         "RTC operation cancelled before execution: " + key);
        }));
        return;
      }
      if constexpr (async_detail::Unwrap<Return>::asynchronous) {
        // Initiation and ready continuations always run on this worker. A room
        // transport thread only fills the reserved slot and wakes the worker.
        Return pending;
        try {
          pending = function();
          if (!pending.valid()) {
            throw std::future_error(std::future_errc::no_state);
          }
        } catch (...) {
          deliver(control_boundary([&]() -> Value { throw; }));
          return;
        }
        std::move(pending).then(
            [this, execution](auto task) {
              try {
                Task ready = std::move(task);
                std::lock_guard lock(mutex_);
                execution->ready = std::move(ready);
                changed_.notify_all();
              } catch (...) {
                report_failure(std::current_exception());
                throw;
              }
            },
            [this, deliver = std::move(deliver)](AsyncResult<Value> ready) mutable {
              // TLS diagnostics from an unrelated command must never leak into
              // this deferred completion. Exceptions carry their own FailureInfo.
              control_failure.reset();
              try {
                deliver(control_boundary([&] { return ready.get(); }));
              } catch (...) {
                report_failure(std::current_exception());
              }
            });
      } else {
        deliver(control_boundary([&] { return function(); }));
      }
    };
    keys_.insert(key);
    try {
      queue_.push_back({std::move(work), execution, false, barrier});
    } catch (...) {
      keys_.erase(key);
      throw;
    }
    ++outstanding_;
    lease->owner = this;
    execution->owner = this;
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
    if (!task) {
      set_async_maintenance({});
      return;
    }
    set_async_maintenance([task = std::move(task)] {
      CompletionSource<ErrorCode> completed;
      auto result = completed.get_result();
      task();
      completed.set_value(ErrorCode::ok);
      return result;
    });
  }

  void set_async_maintenance(Maintenance task) {
    auto handler = task ? std::make_shared<Maintenance>(std::move(task)) : nullptr;
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
    std::shared_ptr<Execution> execution;
    bool cancelled = false;
    bool barrier = false;
  };

  static std::string lane_for(const std::string& key) {
    const auto colon = key.find(':');
    if (colon != std::string::npos) {
      if (key.starts_with("publication.")) {
        return "publication" + key.substr(colon);
      }
      if (key.starts_with("subscription.")) {
        return "subscription" + key.substr(colon);
      }
    }
    return key;
  }

  auto runnable_locked() {
    if (maintenance_running_) {
      return queue_.end();
    }
    for (auto* active : active_) {
      if (active && active->fence) {
        return queue_.end();
      }
    }
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
      if (it->execution->fence) {
        return active_count_ == 0 ? it : queue_.end();
      }
      bool busy = false;
      for (auto* active : active_) {
        busy |= active && active->lane == it->execution->lane;
      }
      if (!busy) {
        return it;
      }
    }
    return queue_.end();
  }

  bool has_ready_locked() const {
    if (maintenance_ready_) {
      return true;
    }
    for (auto* active : active_) {
      if (active && active->ready) {
        return true;
      }
    }
    return false;
  }

  bool can_maintain_locked() const {
    return maintenance_pending_ && !maintenance_running_ && !barrier_pending_ && !stopping_ &&
           active_count_ == 0;
  }

  void report_failure(std::exception_ptr error) noexcept {
    if (failure_) {
      try {
        failure_(std::move(error));
      } catch (...) {
      }
    }
  }

  void run() {
    WorkerScope worker_scope;
    for (;;) {
      Work work;
      Task ready;
      std::shared_ptr<Maintenance> maintenance;
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] {
          return (stopping_ && queue_.empty() && active_count_ == 0 && !maintenance_running_) ||
                 has_ready_locked() || can_maintain_locked() || runnable_locked() != queue_.end();
        });
        for (auto* active : active_) {
          if (active && active->ready) {
            ready = std::move(active->ready);
            break;
          }
        }
        if (!ready && maintenance_ready_) {
          ready = std::move(maintenance_ready_);
        }
        if (!ready) {
          if (stopping_ && queue_.empty() && active_count_ == 0 && !maintenance_running_) {
            return;
          }
          if (can_maintain_locked()) {
            maintenance_pending_ = false;
            maintenance = maintenance_;
            maintenance_running_ = bool(maintenance);
          } else {
            auto next = runnable_locked();
            work = std::move(*next);
            queue_.erase(next);
            for (size_t slot = 0; slot < active_.size(); ++slot) {
              if (!active_[slot]) {
                active_[slot] = work.execution.get();
                work.execution->slot = slot;
                ++active_count_;
                break;
              }
            }
            if (work.barrier && !stopping_) {
              control_.interrupted = false;
            }
          }
        }
      }
      try {
        if (ready) {
          ready();
        } else if (maintenance) {
          auto finish = [this] {
            std::lock_guard lock(mutex_);
            maintenance_running_ = false;
            changed_.notify_all();
          };
          try {
            auto pending = (*maintenance)();
            std::move(pending).then(
                [this, finish](auto task) {
                  try {
                    Task next = std::move(task);
                    std::lock_guard lock(mutex_);
                    maintenance_ready_ = std::move(next);
                    changed_.notify_all();
                  } catch (...) {
                    finish();
                    report_failure(std::current_exception());
                    throw;
                  }
                },
                [this, finish](AsyncResult<ErrorCode> result) {
                  try {
                    auto code = result.get();
                    if (code != ErrorCode::ok) {
                      throw SDKError(ErrorDomain::lifecycle, code, "RTC maintenance failed");
                    }
                  } catch (...) {
                    report_failure(std::current_exception());
                  }
                  finish();
                });
          } catch (...) {
            finish();
            throw;
          }
        } else if (work.run) {
          work.run(work.cancelled);
        }
      } catch (...) {
        // RAII releases execution and notification reservations on every failure.
        report_failure(std::current_exception());
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
  std::array<Execution*, 129> active_{};
  size_t active_count_ = 0;
  std::set<std::string> keys_;
  std::shared_ptr<Maintenance> maintenance_;
  Task maintenance_ready_;
  bool maintenance_running_ = false;
  bool maintenance_pending_ = false;
  size_t outstanding_ = 0;
  bool stopping_ = false, terminal_ = false, barrier_pending_ = false;
  std::thread worker_;
};
}  // namespace livekit::sdk::rtc::detail
