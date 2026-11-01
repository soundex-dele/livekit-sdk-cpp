#pragma once
#include "backends/rtc/control_context.hpp"
#include <utoolkit/task_queue/task_queue.h>
#include <optional>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace livekit::sdk::rtc::detail {
// Thin adapter over the existing FIFO worker; no timers or operation scheduler.
class SerialQueue {
 public:
  explicit SerialQueue(const char* name) : queue_(vi::TaskQueue::create(name)) {}

  bool current() const {
    return queue_->isCurrent();
  }

  template <class F>
  void post(F&& fn) {
    queue_->postTask([fn = std::forward<F>(fn)]() mutable {
      CallbackScope scope;
      fn();
    });
  }

  template <class F>
  auto invoke(F&& fn) -> std::invoke_result_t<F> {
    if (current()) {
      return fn();
    }
    using Value = std::invoke_result_t<F>;
    using Stored = std::conditional_t<std::is_void_v<Value>, bool, Value>;

    struct CompletionState {
      std::mutex mutex;
      std::condition_variable changed;
      bool done = false;
      std::optional<Stored> result;
    };

    // Both sides retain the wait state through notification. TaskQueue's legacy
    // synchronous helper borrows stack synchronization objects and can notify
    // after its waiting caller has already returned and destroyed them.
    auto state = std::make_shared<CompletionState>();
    post([state, fn = std::forward<F>(fn)]() mutable {
      if constexpr (std::is_void_v<Value>) {
        fn();
      } else {
        state->result.emplace(fn());
      }
      {
        std::lock_guard lock(state->mutex);
        state->done = true;
      }
      state->changed.notify_one();
    });
    std::unique_lock lock(state->mutex);
    state->changed.wait(lock, [&] { return state->done; });
    if constexpr (!std::is_void_v<Value>) {
      return std::move(*state->result);
    }
  }

  void fence() {
    invoke([] {});
  }

 private:
  std::unique_ptr<vi::TaskQueue> queue_;
};

// A room needs no media lifecycle worker. Media owners create their own queue.
struct RoomExecution {
  SerialQueue control{"rtc.control"};
  SerialQueue notification{"rtc.notification"};

  bool external() const {
    return !control.current() && !notification.current() && !callback_depth;
  }
};
}  // namespace livekit::sdk::rtc::detail
