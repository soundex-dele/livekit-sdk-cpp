#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <utility>

namespace livekit::sdk::detail {
// Owner serializes access. A full channel rejects the push without moving the
// frame, so the upstream node can retain its event and retry after consumption.
template <class T>
class MediaChannel {
 public:
  MediaChannel(size_t capacity, size_t max_bytes) : capacity_(capacity), max_bytes_(max_bytes) {
    if (!capacity || !max_bytes)
      throw std::invalid_argument("media_graph: zero queue limit");
  }

  bool push(T& frame, size_t bytes) {
    if (bytes > max_bytes_)
      throw std::runtime_error("media_graph/output: frame exceeds queue byte limit");
    if (queue_.size() >= capacity_ || bytes > max_bytes_ - bytes_)
      return false;
    queue_.emplace_back(std::move(frame), bytes);
    bytes_ += bytes;
    return true;
  }

  std::optional<T> pop_due(int64_t time, uint64_t generation) {
    while (!queue_.empty() && queue_.front().first.generation != generation)
      pop();
    if (queue_.empty() || queue_.front().first.timestamp_us > time)
      return {};
    return pop();
  }

  void clear() {
    queue_.clear();
    bytes_ = 0;
  }

  size_t size() const {
    return queue_.size();
  }

 private:
  T pop() {
    auto result = std::move(queue_.front());
    queue_.pop_front();
    bytes_ -= result.second;
    return std::move(result.first);
  }

  size_t capacity_, max_bytes_, bytes_ = 0;
  std::deque<std::pair<T, size_t>> queue_;
};
}  // namespace livekit::sdk::detail
