#pragma once
#include "engines/rtc/command.hpp"
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace livekit::sdk::detail {
// One lazily-created timer per Room, independent of signaling/SDP waits. Holds
// weak operations only; timeout completes promises, never application callbacks.
class OperationDeadlines {
 public:
  OperationDeadlines() : worker_([this](std::stop_token stop) { run(stop); }) {}
  ~OperationDeadlines() { worker_.request_stop(); changed_.notify_all(); }
  void add(const std::shared_ptr<Command>& command) {
    std::lock_guard lock(mutex_);
    for (auto it = deadlines_.begin(); it != deadlines_.end();) {
      auto pending = it->second.lock();
      if (!pending || pending->done()) it = deadlines_.erase(it); else ++it;
    }
    if (deadlines_.size() >= 1024)
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted, "Too many outstanding operations");
    deadlines_.emplace(command->deadline, command); ++revision_; changed_.notify_all();
  }
 private:
  void run(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
      if (deadlines_.empty()) { changed_.wait(lock, stop, [&] { return !deadlines_.empty(); }); continue; }
      const auto revision = revision_;
      const auto deadline = deadlines_.begin()->first;
      if (std::chrono::steady_clock::now() < deadline) {
        changed_.wait_until(lock, stop, deadline, [&] { return revision_ != revision; }); continue;
      }
      auto command = deadlines_.begin()->second.lock(); deadlines_.erase(deadlines_.begin());
      lock.unlock();
      if (command) command->expire(std::chrono::steady_clock::now());
      command.reset(); lock.lock();
    }
  }
  std::mutex mutex_;
  std::condition_variable_any changed_;
  std::multimap<std::chrono::steady_clock::time_point, std::weak_ptr<Command>> deadlines_;
  uint64_t revision_ = 0;
  std::jthread worker_; // destroyed/joined before mutex/condition_variable
};
}
