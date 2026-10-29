#pragma once
#include "livekit/rtc/media_types.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>
#include <variant>

namespace livekit::sdk::rtc::detail {
// Each consumer owns an independent bounded queue, while payloads are immutable
// and shared. A slow consumer never retains more than its configured budget.
class RawFrameQueue final {
 public:
  struct Sample {
    std::variant<std::shared_ptr<const VideoFrame>, std::shared_ptr<const PcmAudioFrame>> payload;
    SteadyClock::time_point captured;
    size_t bytes = 0;
    uint64_t generation = 0;
    uint64_t timestamp = 0;  // Preserve the capture sample clock across fanout.
  };

  explicit RawFrameQueue(QueueLimits limits) : limits_(limits) {
    if (!limits.max_frames || !limits.max_bytes || limits.max_age.count() <= 0) {
      throw std::invalid_argument("Invalid shared raw-frame queue limits");
    }
  }

  void start() {
    std::lock_guard lock(mutex_);
    queue_.clear();
    bytes_ = 0;
    ++generation_;
    active_ = true;
    changed_.notify_all();
  }

  void stop() {
    std::lock_guard lock(mutex_);
    active_ = false;
    queue_.clear();
    bytes_ = 0;
    changed_.notify_all();
  }

  uint64_t generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
  }

  bool write(Sample sample) {
    std::lock_guard lock(mutex_);
    expire();
    if (!active_ || sample.bytes > limits_.max_bytes ||
        SteadyClock::now() - sample.captured > limits_.max_age) {
      return false;
    }
    // Retain recent live media; each branch discards only its own stale backlog.
    while (!queue_.empty() &&
           (queue_.size() >= limits_.max_frames || bytes_ > limits_.max_bytes - sample.bytes)) {
      pop();
    }
    sample.generation = generation_;
    bytes_ += sample.bytes;
    queue_.push_back(std::move(sample));
    changed_.notify_one();
    return true;
  }

  std::optional<Sample> read(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, stop, [&] { return !queue_.empty() || !active_; });
    expire();
    if (stop.stop_requested() || !active_ || queue_.empty()) {
      return {};
    }
    auto result = std::move(queue_.front());
    pop();
    return result;
  }

 private:
  void pop() {
    bytes_ -= queue_.front().bytes;
    queue_.pop_front();
  }

  void expire() {
    const auto now = SteadyClock::now();
    while (!queue_.empty() && now - queue_.front().captured > limits_.max_age) {
      pop();
    }
  }

  const QueueLimits limits_;
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::deque<Sample> queue_;
  size_t bytes_ = 0;
  uint64_t generation_ = 0;
  bool active_ = false;
};
}  // namespace livekit::sdk::rtc::detail
