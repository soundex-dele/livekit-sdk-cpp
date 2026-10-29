#pragma once
#include "support/wake_event.hpp"
#include <memory>
#include <set>
#include <stdexcept>

namespace livekit::sdk::detail {
// Coalesced work notifications, bounded by explicitly registered identities.
// Owners use fresh keys for each incarnation. Retained producer wake handles
// become inert after forget/clear or owner destruction; they retain no owner.
template <class Key>
class ReadySet : public std::enable_shared_from_this<ReadySet<Key>> {
 public:
  ReadySet(std::shared_ptr<WakeEvent> owner, size_t capacity)
      : owner_(std::move(owner)), capacity_(capacity) {
    if (!owner_ || !capacity_) {
      throw std::invalid_argument("Ready-set requires an owner and positive capacity");
    }
  }

  std::shared_ptr<WakeEvent> watch(const Key& key) {
    auto signal = std::make_shared<WakeEvent>([weak = this->weak_from_this(), key] {
      if (auto state = weak.lock()) {
        state->mark(key);
      }
    });
    std::lock_guard lock(mutex_);
    if (registered_.contains(key)) {
      throw std::logic_error("Duplicate ready-set registration");
    }
    if (registered_.size() >= capacity_) {
      throw std::length_error("Ready-set registration capacity exhausted");
    }
    registered_.insert(key);
    return signal;
  }

  void mark(const Key& key) {
    bool changed = false;
    {
      std::lock_guard lock(mutex_);
      if (registered_.contains(key)) {
        changed = ready_.insert(key).second;
      }
    }
    if (changed) {
      owner_->signal();
    }
  }

  std::set<Key> take() {
    std::set<Key> result;
    std::lock_guard lock(mutex_);
    ready_.swap(result);
    return result;
  }

  void forget(const Key& key) {
    std::lock_guard lock(mutex_);
    registered_.erase(key);
    ready_.erase(key);
  }

  void clear() {
    std::lock_guard lock(mutex_);
    registered_.clear();
    ready_.clear();
  }

  size_t pending() const {
    std::lock_guard lock(mutex_);
    return ready_.size();
  }

 private:
  std::shared_ptr<WakeEvent> owner_;
  const size_t capacity_;
  mutable std::mutex mutex_;
  std::set<Key> registered_, ready_;
};
}  // namespace livekit::sdk::detail
