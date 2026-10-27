#pragma once
#include "livekit/audio_stream.hpp"

namespace livekit::sdk::detail {
class AudioFrameOutput {
 public:
  explicit AudioFrameOutput(std::shared_ptr<AudioFrameStream> stream);
  ~AudioFrameOutput();
  AudioFrameOutput(const AudioFrameOutput&) = delete;
  AudioFrameOutput& operator=(const AudioFrameOutput&) = delete;
  uint64_t add(const std::string& track_id);
  void remove(const std::string& track_id, uint64_t lease);
  void submit(const std::string& track_id, uint64_t lease, const void* data, int bits,
      int sample_rate, std::size_t channels, std::size_t frames, std::optional<int64_t> capture_timestamp_ms);
  void advance();
  void close();
 private:
  std::shared_ptr<AudioFrameStream> stream_;
};
}
