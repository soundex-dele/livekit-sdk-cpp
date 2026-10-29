#pragma once
#include <CoreMedia/CoreMedia.h>
#include <vector>
struct SwrContext;

namespace livekit::sdk::studio_detail {
// One converter per serial capture callback queue; preserves resampling delay
// across packets and recreates the converter when a device changes format.
class MacAudioSamples {
 public:
  MacAudioSamples() = default;
  ~MacAudioSamples();
  MacAudioSamples(const MacAudioSamples&) = delete;
  MacAudioSamples& operator=(const MacAudioSamples&) = delete;
  std::vector<float> convert(CMSampleBufferRef sample);

 private:
  SwrContext* resampler = nullptr;
  int inputRate = 0, inputChannels = 0, inputFormat = -1;
};
}  // namespace livekit::sdk::studio_detail
