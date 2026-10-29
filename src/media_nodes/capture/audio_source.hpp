#pragma once
#include "livekit/audio_source.hpp"
#include <stop_token>

namespace livekit::sdk::detail {
class PcmSourceWriter {
 public:
  // Reject an in-flight device callback if the reader was reset meanwhile.
  static AudioWriteStatus write(PcmAudioSource& source,const PcmInputFrame& frame,uint64_t generation) {
    return source.write(frame,generation);
  }
};
struct PcmSourceBlock {
  std::vector<int16_t> samples;
  std::chrono::steady_clock::time_point captured;
  uint64_t generation = 0;
  bool silence = false;
};
class PcmSourceReader {
 public:
  explicit PcmSourceReader(std::shared_ptr<PcmAudioSource> source);
  ~PcmSourceReader();
  PcmSourceReader(const PcmSourceReader&) = delete;
  PcmSourceReader& operator=(const PcmSourceReader&) = delete;
  void start();
  void stop();
  // Waits only for the very first input. Thereafter produces one 10ms slot,
  // including explicit silence for gaps/underflow. Native caller paces reads.
  std::optional<PcmSourceBlock> read(std::stop_token stop = {});
  void skip(size_t blocks); // scheduler stall: advance instead of bursting
 private:
  std::shared_ptr<PcmAudioSource> source_;
};
}
