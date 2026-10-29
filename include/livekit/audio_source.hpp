#pragma once
#include "livekit/audio_frame.hpp"

namespace livekit::sdk {
namespace detail { class PcmSourceReader; class PcmSourceWriter; }
struct PcmSourceOptions {
  int sample_rate = 48000;
  size_t channels = 1;
  AudioSampleFormat format = AudioSampleFormat::pcm16;
  std::chrono::milliseconds max_queue_duration{200};
  size_t max_bytes = 1024 * 1024;
  bool drift_compensation=false; // Requires capture_time on successive writes.
  int max_drift_ppm=1000;
};
// Interleaved PCM, 1 sample..100 ms per write, buffered into 10 ms blocks. Exactly one
// sample vector is populated. Format/rate/channels are fixed by the source.
struct PcmInputFrame {
  std::vector<int16_t> samples;
  std::vector<float> float_samples; // finite, normalized [-1, 1]
  // Time of the FIRST sample on std::chrono::steady_clock. Omit for a
  // continuous sample timeline. Explicit times must not overlap earlier writes.
  std::optional<std::chrono::steady_clock::time_point> capture_time;
};
enum class AudioWriteStatus { accepted, backpressure, closed };
struct AudioSourceStats {
  size_t queued_blocks = 0, queued_bytes = 0;
  // All sample counters count per-channel samples at the INPUT sample rate.
  uint64_t accepted_samples = 0, consumed_samples = 0, dropped_samples = 0;
  uint64_t silence_samples = 0, skipped_samples = 0, rejected_writes = 0, generation = 0;
  bool attached = false, accepting = false, closed = false;
  double estimated_drift_ppm=0;
  int64_t corrected_samples=0;
};
class PcmAudioSource {
 public:
  explicit PcmAudioSource(PcmSourceOptions options = {});
  ~PcmAudioSource();
  PcmAudioSource(const PcmAudioSource&) = delete;
  PcmAudioSource& operator=(const PcmAudioSource&) = delete;
  PcmSourceOptions options() const;
  // Atomic admission of the whole write; no waiting for queue space. A caller
  // may retry backpressure. No input or sample position is consumed on rejection.
  AudioWriteStatus write(const PcmInputFrame& frame);
  AudioSourceStats stats() const;
  // Permanent EOS discards pending samples. Unpublish separately. An already
  // consumed block may still be in the encoder/transport.
  void close();
 private:
  friend class detail::PcmSourceReader;
  friend class detail::PcmSourceWriter;
  AudioWriteStatus write(const PcmInputFrame&,std::optional<uint64_t> expected_generation);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
