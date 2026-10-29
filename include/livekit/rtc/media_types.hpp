#pragma once
#include "livekit/rtc/types.hpp"
#include "livekit/media.hpp"

namespace livekit::sdk::rtc {
enum class PcmFormat { s16, f32 };

struct PcmAudioFrame {
  uint32_t sample_rate = 48000;
  uint16_t channels = 1;
  PcmFormat format = PcmFormat::s16;
  // Interleaved samples. Populate exactly one vector according to format.
  std::vector<int16_t> samples;
  std::vector<float> float_samples;

  size_t samples_per_channel() const {
    return channels ? (format == PcmFormat::s16 ? samples.size() : float_samples.size()) / channels
                    : 0;
  }
};

struct VideoCodecOptions {
  uint32_t fps = 30;
  size_t max_frames = 2, max_bytes = 32 * 1024 * 1024;
  std::chrono::milliseconds max_age{250};
  uint32_t max_width = 4096, max_height = 4096;
};

struct VideoCodecSnapshot {
  uint64_t admitted = 0, completed = 0, dropped = 0;
  size_t queued_frames = 0, queued_bytes = 0;
  std::string error;
};

struct AudioCodecOptions {
  size_t max_frames = 25, max_bytes = 256 * 1024;
  std::chrono::milliseconds max_age{250};
  uint32_t packet_duration_ms = 20;  // Encode: 10, 20, 40 or 60.
  bool jitter_buffer = false;        // Receive: NetEq pacing, adaptive jitter and PLC.
  std::chrono::milliseconds jitter_min_delay{20}, jitter_max_delay{200};
  uint32_t bitrate_bps = 64000;      // Encode: 6000..510000.
};

struct AudioCodecSnapshot {
  uint64_t admitted = 0, completed = 0, dropped = 0, discontinuities = 0, invalid_packets = 0;
  size_t queued_frames = 0, queued_bytes = 0;
  uint64_t concealed_samples = 0, accelerated_samples = 0, decelerated_samples = 0;
  uint64_t jitter_buffer_ms = 0;
  std::string error;
};

struct MediaInputSnapshot {
  uint64_t emitted = 0, dropped = 0;
  bool active = false, finished = false;
  std::string error;
};

}  // namespace livekit::sdk::rtc
