#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace livekit::sdk::detail {
// Workers capture only State, so destruction from a worker never accesses the
// executor object after it dies. Pending callbacks are discarded at shutdown.
class TaskExecutor {
  struct State {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> tasks;
    bool stopped = false;
  };
  std::shared_ptr<State> state_ = std::make_shared<State>();
  std::vector<std::thread> workers_;
  size_t capacity_;
 public:
  explicit TaskExecutor(size_t workers = 4, size_t capacity = 64) : capacity_(capacity) {
    try {
      for (size_t i=0;i<workers;++i) workers_.emplace_back([s=state_] {
        for (;;) {
          std::function<void()> task;
          { std::unique_lock lock(s->mutex); s->ready.wait(lock,[&]{return s->stopped||!s->tasks.empty();});
            if(s->stopped)return; task=std::move(s->tasks.front());s->tasks.pop_front(); }
          try { task(); } catch (...) { /* task owns its error channel */ }
        }
      });
    } catch (...) { stop(); throw; }
  }
  ~TaskExecutor(){stop();}
  bool post(std::function<void()> task) {
    std::lock_guard lock(state_->mutex);
    if(state_->stopped||state_->tasks.size()>=capacity_)return false;
    state_->tasks.push_back(std::move(task));state_->ready.notify_one();return true;
  }
  void stop() {
    {std::lock_guard lock(state_->mutex);state_->stopped=true;state_->tasks.clear();state_->ready.notify_all();}
    for(auto& t:workers_)if(t.joinable()){if(t.get_id()==std::this_thread::get_id())t.detach();else t.join();}
  }
};
}
