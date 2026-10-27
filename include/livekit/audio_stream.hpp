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
namespace detail { class AudioFrameOutput; }
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
    return std::chrono::microseconds(sample_rate > 0 ? samples_per_channel * 1000000ull / sample_rate : 0);
  }
};
struct AudioStreamOptions {
  std::size_t max_frames = 50, max_bytes = 4 * 1024 * 1024;
  AudioSampleFormat format = AudioSampleFormat::pcm16;
};
enum class AudioReadStatus { frame, timeout, cancelled, closed };
struct AudioReadResult {
  AudioReadStatus status = AudioReadStatus::timeout;
  std::shared_ptr<const AudioFrame> frame;
};
struct AudioStreamStats {
  std::size_t queued_frames = 0, queued_bytes = 0, tracks = 0;
  uint64_t received = 0, dropped = 0, dropped_samples = 0, generation = 0;
  bool closed = false;
};
// Single logical Room session. Queue overload drops oldest complete blocks;
// sequence/sample_index expose gaps, never silently stretch the audio timeline.
class AudioFrameStream {
 public:
  explicit AudioFrameStream(AudioStreamOptions options = {});
  ~AudioFrameStream();
  AudioFrameStream(const AudioFrameStream&) = delete;
  AudioFrameStream& operator=(const AudioFrameStream&) = delete;
  AudioReadResult read(std::chrono::milliseconds timeout, std::stop_token stop = {});
  // Consumes only this track, leaving other tracks queued. A removed track
  // waits for a new subscription or timeout; Room callbacks report removal.
  AudioReadResult read_for(const std::string& track_id, std::chrono::milliseconds timeout, std::stop_token stop = {});
  AudioStreamStats stats() const;
  void close();
 private:
  friend class detail::AudioFrameOutput;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
