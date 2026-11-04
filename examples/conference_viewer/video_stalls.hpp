#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace demo {
// Presentation-output arrivals, measured with a monotonic clock. This does not
// measure GPU presentation or use media timestamps as a wall clock.
class VideoStalls {
 public:
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;
  static constexpr std::chrono::milliseconds threshold{300};

  struct Sample {
    double observed_seconds = 0, stalled_seconds = 0;
    uint64_t count = 0;
    bool active = false, waiting = true, stalled = false;
    std::optional<double> rate_percent;
  };

  void set_active(bool active, Time now = Clock::now()) {
    if (active_ == active) {
      return;
    }
    if (!active) {
      finish_interval(now);
      last_frame_.reset();
    }
    active_ = active;
  }

  void frame(uint64_t generation, Time now = Clock::now()) {
    if (!active_) {
      return;
    }
    if (generation_ && *generation_ != generation) {
      observed_ = stalled_ = Clock::duration::zero();
      count_ = 0;
      last_frame_.reset();
    }
    generation_ = generation;
    finish_interval(now);
    last_frame_ = now;
  }

  Sample sample(Time now = Clock::now()) const {
    auto observed = observed_, stalled = stalled_;
    auto count = count_;
    bool stalling = false;
    if (active_ && last_frame_) {
      const auto gap = std::max(now - *last_frame_, Clock::duration::zero());
      observed += gap;
      if (gap > threshold) {
        stalled += gap - threshold;
        ++count;
        stalling = true;
      }
    }
    Sample result;
    result.observed_seconds = std::chrono::duration<double>(observed).count();
    result.stalled_seconds = std::chrono::duration<double>(stalled).count();
    result.count = count;
    result.active = active_;
    result.waiting = !last_frame_;
    result.stalled = stalling;
    if (observed > Clock::duration::zero()) {
      result.rate_percent = 100.0 * result.stalled_seconds / result.observed_seconds;
    }
    return result;
  }

 private:
  void finish_interval(Time now) {
    if (!last_frame_) {
      return;
    }
    const auto gap = std::max(now - *last_frame_, Clock::duration::zero());
    observed_ += gap;
    if (gap > threshold) {
      stalled_ += gap - threshold;
      ++count_;
    }
  }

  bool active_ = false;
  std::optional<Time> last_frame_;
  std::optional<uint64_t> generation_;
  Clock::duration observed_{}, stalled_{};
  uint64_t count_ = 0;
};
}  // namespace demo
