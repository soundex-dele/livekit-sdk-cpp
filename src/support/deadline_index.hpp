#pragma once
#include <chrono>
#include <map>
#include <optional>
#include <vector>

namespace livekit::sdk::detail {
// Owner-sequence index with exactly one live deadline per identity. Updating or
// cancelling erases the old entry immediately: no stale heap entries grow with
// media rate or repeated control changes.
template <class Key>
class DeadlineIndex {
 public:
  using Time = std::chrono::steady_clock::time_point;
  DeadlineIndex() = default;
  DeadlineIndex(const DeadlineIndex&) = delete;
  DeadlineIndex& operator=(const DeadlineIndex&) = delete;

  void set(const Key& key, std::optional<Time> time) {
    const auto current = by_key_.find(key);
    if (current != by_key_.end() && time && current->second->first == *time) {
      return;
    }
    erase(key);
    if (time) {
      const auto position = by_time_.emplace(*time, key);
      try {
        by_key_.emplace(key, position);
      } catch (...) {
        by_time_.erase(position);
        throw;
      }
    }
  }

  void erase(const Key& key) {
    const auto found = by_key_.find(key);
    if (found != by_key_.end()) {
      by_time_.erase(found->second);
      by_key_.erase(found);
    }
  }

  std::vector<Key> take_due(Time now) {
    std::vector<Key> result;
    while (!by_time_.empty() && by_time_.begin()->first <= now) {
      const auto found = by_time_.begin();
      result.push_back(found->second);
      by_key_.erase(found->second);
      by_time_.erase(found);
    }
    return result;
  }

  std::optional<Time> next() const {
    return by_time_.empty() ? std::optional<Time>{} : by_time_.begin()->first;
  }

  size_t size() const {
    return by_key_.size();
  }

  void clear() {
    by_key_.clear();
    by_time_.clear();
  }

 private:
  using Timeline = std::multimap<Time, Key>;
  Timeline by_time_;
  std::map<Key, typename Timeline::iterator> by_key_;
};
}  // namespace livekit::sdk::detail
