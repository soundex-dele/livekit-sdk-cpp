#pragma once
#include "livekit/error.hpp"
#include "utoolkit/task_queue/task_queue.h"
#include <functional>
#include <memory>
#include <mutex>

namespace livekit::sdk::detail {
// Borrowed queue with a shared submission fence. Producers may retain copies
// after the queue owner disappears; close() rejects them before queue teardown.
// Accepted tasks must capture their own lifetime guard or be drained by owner.
class TaskDispatcher {
  struct State {
    std::mutex mutex;
    vi::TaskQueue* queue = nullptr;
  };

  std::shared_ptr<State> state_ = std::make_shared<State>();

 public:
  void bind(vi::TaskQueue& queue) {
    std::lock_guard lock(state_->mutex);
    state_->queue = &queue;
  }

  void close() {
    std::lock_guard lock(state_->mutex);
    state_->queue = nullptr;
  }

  void operator()(std::function<void()> task) const {
    std::lock_guard lock(state_->mutex);
    if (!state_->queue) {
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed,
                     "Task dispatcher is closed");
    }
    state_->queue->postTask(std::move(task));
  }
};
}  // namespace livekit::sdk::detail
