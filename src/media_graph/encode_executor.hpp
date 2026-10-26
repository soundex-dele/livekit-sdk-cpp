#pragma once
#include "api/environment/environment.h"
#include "api/task_queue/task_queue_factory.h"
#include "absl/functional/any_invocable.h"
#include <algorithm>
#include <deque>
#include <future>
#include <mutex>

namespace livekit::sdk::detail {
// Graph-owned execution sequence. Only one pump task is posted to the native
// thread; raw media never creates an unbounded native task-queue backlog.
class EncodeExecutor {
 public:
  using Work = absl::AnyInvocable<void() &&>;
  EncodeExecutor(const webrtc::Environment& env, const char* name, size_t capacity)
      : capacity_(capacity), queue_(env.task_queue_factory().CreateTaskQueue(
            name, webrtc::TaskQueueFactory::Priority::NORMAL)) {}
  ~EncodeExecutor() { flush(); queue_.reset(); }
  // Returns the number of old media jobs evicted. Control jobs are never lost.
  size_t post(Work work, bool media = false) {
    std::lock_guard lock(mutex_);
    size_t dropped = 0;
    if (media && media_count_ == capacity_) {
      auto it = std::find_if(jobs_.begin(), jobs_.end(), [](auto& j) { return j.media; });
      jobs_.erase(it); --media_count_; dropped = 1;
    }
    jobs_.push_back({std::move(work), media});
    if (media) ++media_count_;
    if (!scheduled_) {
      scheduled_ = true;
      queue_->PostTask([this] { drain(); });
    }
    return dropped;
  }
  void flush() {
    std::promise<void> done;
    auto future = done.get_future();
    post([&done] { done.set_value(); });
    future.get();
  }
 private:
  struct Job { Work work; bool media; };
  void drain() {
    for (;;) {
      Work work;
      {
        std::lock_guard lock(mutex_);
        if (jobs_.empty()) { scheduled_ = false; return; }
        auto job = std::move(jobs_.front()); jobs_.pop_front();
        if (job.media) --media_count_;
        work = std::move(job.work);
      }
      std::move(work)();
    }
  }
  const size_t capacity_;
  std::mutex mutex_;
  std::deque<Job> jobs_;
  size_t media_count_ = 0;
  bool scheduled_ = false;
  std::unique_ptr<webrtc::TaskQueueBase, webrtc::TaskQueueDeleter> queue_;
};
}
