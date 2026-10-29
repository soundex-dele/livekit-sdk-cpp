#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace livekit::sdk::media {
class PresentationClock {
 public:
  virtual ~PresentationClock() = default;
  virtual void observe_audio(int64_t sender_time_us,
                             std::chrono::steady_clock::time_point presentation,
                             uint64_t epoch) = 0;
};

// Timing is separate from immutable storage: the same captured payload can be
// published in multiple rooms with different epochs and RTP clocks.
struct FrameTiming {
  uint64_t timestamp = 0;
  uint32_t clock_rate = 0;
  uint64_t epoch = 0;
  std::chrono::steady_clock::time_point captured = std::chrono::steady_clock::now();
  std::optional<int64_t> sender_capture_time_us;
  std::optional<std::chrono::steady_clock::time_point> presentation_time;
  std::shared_ptr<PresentationClock> presentation_clock;
};

enum class FrameFormat { i420, rgba8, pcm_s16, pcm_f32 };

// Owns the object backing the spans. Copies share storage; never mutate the
// backing object after publishing a view. Conversion creates a new payload.
struct FrameView {
  std::shared_ptr<const void> owner;
  FrameFormat format = FrameFormat::i420;
  std::vector<std::span<const std::byte>> planes;
  std::vector<size_t> strides;
  uint32_t width = 0, height = 0, sample_rate = 0;
  uint16_t channels = 0;
  FrameTiming timing;
};
}  // namespace livekit::sdk::media
