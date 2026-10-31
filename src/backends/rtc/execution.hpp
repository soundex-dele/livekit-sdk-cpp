#pragma once
#include "backends/rtc/control_context.hpp"
#include <utoolkit/task_queue/task_queue.h>
#include <cstdio>
#include <future>
#include <memory>
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
      try {
        fn();
      } catch (const std::exception& error) {
        std::fprintf(stderr, "RTC worker task failed: %s\n", error.what());
      } catch (...) {
        std::fprintf(stderr, "RTC worker task failed\n");
      }
    });
  }

  template <class F>
  auto invoke(F&& fn) -> std::invoke_result_t<F> {
    if (current()) {
      return fn();
    }
    auto task =
        std::make_shared<std::packaged_task<std::invoke_result_t<F>()>>(std::forward<F>(fn));
    auto result = task->get_future();
    queue_->postTask([task] { (*task)(); });
    return result.get();
  }

  void fence() {
    invoke([] {});
  }

 private:
  std::unique_ptr<vi::TaskQueue> queue_;
};

struct RtcExecution {
  SerialQueue control{"rtc.control"};
  SerialQueue lifecycle{"rtc.lifecycle"};
  SerialQueue notification{"rtc.notification"};

  bool external() const {
    return !control.current() && !lifecycle.current() && !notification.current() && !callback_depth;
  }
};
}  // namespace livekit::sdk::rtc::detail
