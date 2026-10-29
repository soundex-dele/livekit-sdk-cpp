#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace livekit::sdk {
enum class AudioSampleFormat { pcm16, float32 };

// One decoded remote track, interleaved channels. Exactly one sample vector is
// populated, according to format. This is before application mixing/playout.
struct AudioFrame {
  std::string track_id;
  AudioSampleFormat format = AudioSampleFormat::pcm16;
  int sample_rate = 0;
  std::size_t channels = 0, samples_per_channel = 0;
  std::vector<int16_t> samples;
  std::vector<float> float_samples;
  uint64_t stream_generation = 0, track_generation = 0, sequence = 0;
  // Decoded sample position per channel, including blocks dropped by this
  // queue. Resets at a stream/track/format boundary. Not an RTP timestamp.
  uint64_t sample_index = 0;
  bool format_changed = false;
  // Native WebRTC capture estimate, when available, on the WebRTC TimeMillis
  // clock. Not UTC, a C++ steady_clock epoch, or actual audio playout time.
  std::optional<int64_t> capture_timestamp_ms;

  std::chrono::microseconds duration() const {
    return std::chrono::microseconds(
        sample_rate > 0 ? samples_per_channel * 1000000ull / sample_rate : 0);
  }
};
}  // namespace livekit::sdk
