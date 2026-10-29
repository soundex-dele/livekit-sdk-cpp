#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace livekit::sdk::rtc {
struct AudioProcessingOptions {
  bool enabled = true;
  bool echo_cancellation = true;
  bool noise_suppression = true;
  bool automatic_gain = true;
  // Device/capture buffering not already represented by the reference timestamp.
  std::chrono::milliseconds device_delay{40};
};

// Bounded, multi-reader history of the actual mixed speaker signal. Render
// writes neither allocate nor wait for a lock. Each capture processor has its
// own cursor; sharing capture processes once before fanout to multiple rooms.
class AudioRenderReference final {
 public:
  struct Block {
    std::array<int16_t, 960> stereo{};
    std::chrono::steady_clock::time_point rendered;
    uint64_t sequence = 0;
  };

  void write(std::span<const int16_t> stereo) noexcept {
    if (stereo.size() != 960) {
      return;
    }
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock) {
      ++dropped_;
      return;
    }
    auto& block = blocks_[sequence_ % blocks_.size()];
    std::copy(stereo.begin(), stereo.end(), block.stereo.begin());
    block.rendered = std::chrono::steady_clock::now();
    block.sequence = ++sequence_;
  }

  std::vector<Block> read_since(uint64_t& cursor) const {
    std::lock_guard lock(mutex_);
    std::vector<Block> result;
    const auto begin =
        std::max(cursor, sequence_ > blocks_.size() ? sequence_ - blocks_.size() : 0);
    result.reserve(size_t(sequence_ - begin));
    for (auto i = begin; i < sequence_; ++i) {
      result.push_back(blocks_[i % blocks_.size()]);
    }
    cursor = sequence_;
    return result;
  }

  uint64_t dropped() const {
    return dropped_;
  }

 private:
  mutable std::mutex mutex_;
  std::array<Block, 32> blocks_{};
  uint64_t sequence_ = 0;
  std::atomic<uint64_t> dropped_{0};
};
}  // namespace livekit::sdk::rtc
