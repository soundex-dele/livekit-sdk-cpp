#pragma once
#include "backends/rtc/control_context.hpp"
#include "livekit/async_result.hpp"
#include "support/task_dispatcher.hpp"
#include "utoolkit/task_queue/task_queue.h"

namespace livekit::sdk::rtc::detail {
// Only admitted controls (at most 129) and one coalesced maintenance operation
// can schedule work here. Network continuations retain the same admission slot.
// The control scheduler never waits for graph/device lifecycle work.
class LifecycleExecutor {
 public:
  LifecycleExecutor() : queue_(vi::TaskQueue::create("rtc.lifecycle")) {
    dispatch_.bind(*queue_);
  }

  ~LifecycleExecutor() {
    shutdown();
  }

  // Settlement can wake another thread before the finishing task has released
  // its captures. Join that tail before destroying graphs, media or room state.
  void shutdown() {
    dispatch_.close();
    queue_.reset();
  }

  auto dispatcher() const {
    return [dispatch = dispatch_](auto task) {
      dispatch([task = std::move(task)]() mutable {
        WorkerScope scope;
        task();
      });
    };
  }

  template <class Function>
  auto submit(Function function) {
    CompletionSource<void> ready;
    auto result = ready.get_result();
    ready.set_value();
    return std::move(result).then(
        dispatcher(), [function = std::move(function)](AsyncResult<void> ready) mutable {
          ready.get();
          control_failure.reset();
          if constexpr (std::is_same_v<std::invoke_result_t<Function>, ErrorCode>) {
            auto code = function();
            if (code != ErrorCode::ok) {
              if (control_failure && control_failure->code == code) {
                throw SDKError(control_failure);
              }
              throw SDKError(ErrorDomain::lifecycle, code, "RTC lifecycle operation failed");
            }
            return code;
          } else {
            return function();
          }
        });
  }

 private:
  std::unique_ptr<vi::TaskQueue> queue_;
  livekit::sdk::detail::TaskDispatcher dispatch_;
};
}  // namespace livekit::sdk::rtc::detail
