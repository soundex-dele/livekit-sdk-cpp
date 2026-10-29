#pragma once
#include <chrono>
#include <cstdint>

namespace livekit::sdk::media {
// Serialized by the owner. Steady clock is the initial playback master clock.
class MediaClock {
 public:
  int64_t position_us() const {
    return position_ + (running_ ? std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - anchor_)
                                       .count()
                                 : 0);
  }

  void reset(int64_t position, bool running = false) {
    position_ = position;
    anchor_ = std::chrono::steady_clock::now();
    running_ = running;
  }

  void pause() {
    reset(position_us());
  }

  void resume() {
    reset(position_us(), true);
  }

 private:
  int64_t position_ = 0;
  bool running_ = false;
  std::chrono::steady_clock::time_point anchor_{};
};
}  // namespace livekit::sdk::media
