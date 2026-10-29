#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace livekit::sdk::detail {
// Cross-thread channel for owned payloads. Raw video may discard oldest;
// dependent encoded frames/audio use reject-new and report discontinuity.
template <class T>
class RealtimeChannel {
 public:
  RealtimeChannel(size_t capacity, size_t bytes, bool latest)
      : capacity_(capacity), max_bytes_(bytes), latest_(latest) {
    if (!capacity || !bytes)
      throw std::invalid_argument("media_graph: zero queue limit");
  }

  bool push(T value, size_t bytes, uint64_t generation) {
    std::lock_guard lock(mutex_);
    if (closed_ || generation != generation_ || bytes > max_bytes_) {
      ++dropped_;
      return false;
    }
    while (queue_.size() >= capacity_ || bytes_ + bytes > max_bytes_) {
      if (!latest_) {
        ++dropped_;
        return false;
      }
      bytes_ -= queue_.front().bytes;
      queue_.pop_front();
      ++dropped_;
    }
    queue_.push_back({std::move(value), bytes});
    bytes_ += bytes;
    ready_.notify_one();
    return true;
  }

  std::optional<T> wait_pop() {
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [&] { return closed_ || !queue_.empty(); });
    if (queue_.empty())
      return {};
    auto item = std::move(queue_.front());
    queue_.pop_front();
    bytes_ -= item.bytes;
    return std::move(item.value);
  }

  void reset(uint64_t generation) {
    std::lock_guard lock(mutex_);
    queue_.clear();
    bytes_ = 0;
    generation_ = generation;
  }

  void close(bool drain = false) {
    std::lock_guard lock(mutex_);
    closed_ = true;
    if (!drain) {
      queue_.clear();
      bytes_ = 0;
    }
    ready_.notify_all();
  }

  // Owner sequence only, after the previous producer/consumer have joined.
  void reopen() {
    std::lock_guard lock(mutex_);
    queue_.clear();
    bytes_ = 0;
    closed_ = false;
  }

  size_t dropped() const {
    std::lock_guard lock(mutex_);
    return dropped_;
  }

 private:
  struct Item {
    T value;
    size_t bytes;
  };

  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Item> queue_;
  size_t capacity_, max_bytes_, bytes_ = 0, dropped_ = 0;
  uint64_t generation_ = 0;
  bool latest_, closed_ = false;
};
}  // namespace livekit::sdk::detail
