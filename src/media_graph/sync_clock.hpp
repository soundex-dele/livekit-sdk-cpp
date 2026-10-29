#pragma once
#include "livekit/media_core/frame_view.hpp"
#include <mutex>

namespace livekit::sdk::rtc::detail {
// Shared only by tracks of one remote participant and stream group. An absent
// sender clock is explicitly unsynchronized; unrelated RTP origins are never
// treated as a common clock. Epoch changes invalidate the previous anchor.
class MediaSyncClock final : public media::PresentationClock,
                             public std::enable_shared_from_this<MediaSyncClock> {
 public:
  explicit MediaSyncClock(std::chrono::milliseconds delay) : delay_(delay) {}

  void map(media::FrameTiming& timing) {
    timing.presentation_time.reset();
    timing.presentation_clock.reset();
    if (!timing.sender_capture_time_us) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex_);
    if (epoch_ && timing.epoch < epoch_) {
      return;
    }
    if (!sender_origin_ || timing.epoch != epoch_) {
      sender_origin_ = timing.sender_capture_time_us;
      local_origin_ = now + delay_;
      epoch_ = timing.epoch;
    }
    auto distance = static_cast<long double>(*timing.sender_capture_time_us) - *sender_origin_;
    if (distance > 60000000 || distance < -60000000) {
      sender_origin_ = timing.sender_capture_time_us;
      local_origin_ = now + delay_;
      distance = 0;
    }
    const auto delta = std::chrono::microseconds(int64_t(distance));
    auto scheduled = local_origin_ + delta;
    if (scheduled > now + std::chrono::seconds(2) || scheduled < now - std::chrono::seconds(2)) {
      sender_origin_ = timing.sender_capture_time_us;
      local_origin_ = now + delay_;
      scheduled = local_origin_;
    }
    timing.presentation_time = scheduled;
    timing.presentation_clock = weak_from_this().lock();
  }

  // The speaker's actual sample cursor becomes the master. This tracks device
  // drift and NetEq time stretching without making video follow packet arrival.
  void observe_audio(int64_t sender_time_us, std::chrono::steady_clock::time_point presentation,
                     uint64_t epoch) override {
    std::lock_guard lock(mutex_);
    if (epoch < epoch_) {
      return;
    }
    sender_origin_ = sender_time_us;
    local_origin_ = presentation;
    epoch_ = epoch;
  }

 private:
  const std::chrono::milliseconds delay_;
  std::mutex mutex_;
  std::optional<int64_t> sender_origin_;
  std::chrono::steady_clock::time_point local_origin_;
  uint64_t epoch_ = 0;
};
}  // namespace livekit::sdk::rtc::detail
