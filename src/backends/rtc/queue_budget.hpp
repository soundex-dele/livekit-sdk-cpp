#pragma once
#include "livekit/rtc/types.hpp"
#include <atomic>
#include <limits>
#include <mutex>

namespace livekit::sdk::rtc::detail {
// A reservation follows one access unit across endpoint and native queues.
// A track budget holds a parent room reservation. Neither byte nor frame
// accounting doubles when ownership moves between those queues.
struct QueueBudget : std::enable_shared_from_this<QueueBudget> {
  explicit QueueBudget(size_t limit, size_t max_frames = std::numeric_limits<size_t>::max(),
                       std::shared_ptr<QueueBudget> parent = {})
      : limit(limit), max_frames(max_frames), parent(std::move(parent)) {}
  const size_t limit, max_frames;
  const std::shared_ptr<QueueBudget> parent;
  std::atomic<size_t> used{0}, frames{0};
  std::mutex mutex;
  bool reserve(size_t bytes) {
    std::lock_guard lock(mutex);
    if (bytes > limit || used > limit - bytes || frames >= max_frames) return false;
    used += bytes; ++frames; return true;
  }
  void release(size_t bytes) { std::lock_guard lock(mutex); used -= bytes; --frames; }
  struct Reservation {
    Reservation(std::shared_ptr<QueueBudget> owner, size_t size, std::shared_ptr<void> parent)
        : budget(std::move(owner)), bytes(size), parent(std::move(parent)) {}
    std::shared_ptr<QueueBudget> budget;
    size_t bytes;
    std::shared_ptr<void> parent;
    ~Reservation() { budget->release(bytes); }
  };
  std::shared_ptr<void> claim(size_t bytes, std::shared_ptr<void> existing_parent = {}) {
    auto owner = shared_from_this();
    if (!reserve(bytes)) return {};
    try {
      if (parent && !existing_parent) {
        existing_parent = parent->claim(bytes);
        if (!existing_parent) { release(bytes); return {}; }
      }
      return std::make_shared<Reservation>(std::move(owner), bytes, std::move(existing_parent));
    } catch (...) { release(bytes); throw; }
  }
  bool charge(EncodedBuffer& buffer) {
    if (buffer.budget_ == this && buffer.reservation_) return true;
    // An initial native receive buffer may only have a room reservation. Adopt
    // it without needing spare room capacity at the endpoint handoff.
    auto existing_parent = parent && buffer.budget_ == parent.get() ? buffer.reservation_ : std::shared_ptr<void>{};
    auto reservation = claim(buffer.size(), std::move(existing_parent));
    if (!reservation) return false;
    const auto admitted = buffer.reservation_ ? buffer.admitted_ : SteadyClock::now();
    buffer.reservation_ = std::move(reservation); buffer.budget_ = this; buffer.admitted_ = admitted;
    return true;
  }
  std::optional<EncodedBuffer> copy(std::span<const uint8_t> bytes) {
    auto reservation = claim(bytes.size());
    if (!reservation) return {};
    EncodedBuffer buffer(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    buffer.reservation_ = std::move(reservation); buffer.budget_ = this; buffer.admitted_ = SteadyClock::now();
    return buffer;
  }
  static SteadyClock::time_point admitted(const EncodedBuffer& buffer) {
    return buffer.reservation_ ? buffer.admitted_ : SteadyClock::now();
  }
  static std::shared_ptr<void> ownership(const EncodedBuffer& buffer) { return buffer.reservation_; }
  static void release_to_caller(EncodedBuffer& buffer) { buffer.reservation_.reset(); buffer.budget_ = nullptr; }
};
} // namespace livekit::sdk::rtc::detail
