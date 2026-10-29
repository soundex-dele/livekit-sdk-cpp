#pragma once
#include "common_audio/resampler/include/push_resampler.h"
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace livekit::sdk::detail {
// Per-publication streaming filter. Caller supplies exactly 10ms of PCM16.
class PcmResampler {
 public:
  PcmResampler(int rate, size_t channels)
      : input_samples_(rate / 100), channels_(channels), output_(480 * channels) {
    if (rate < 8000 || rate > 96000 || rate % 100 || (channels != 1 && channels != 2))
      throw std::invalid_argument("Invalid PCM resampler format");
    reset();
  }

  void reset() {
    resampler_ = std::make_unique<webrtc::PushResampler<int16_t>>(input_samples_, 480, channels_);
  }

  std::span<const int16_t> process(std::span<const int16_t> input) {
    if (input.size() != input_samples_ * channels_)
      throw std::invalid_argument("PCM resampler requires a 10ms block");
    const auto count = resampler_->Resample(
        webrtc::InterleavedView<const int16_t>(input.data(), input_samples_, channels_),
        webrtc::InterleavedView<int16_t>(output_.data(), 480, channels_));
    if (count != static_cast<int>(output_.size()))
      throw std::runtime_error("PCM resampling produced an invalid block");
    return output_;
  }

 private:
  size_t input_samples_, channels_;
  std::vector<int16_t> output_;
  std::unique_ptr<webrtc::PushResampler<int16_t>> resampler_;
};
}  // namespace livekit::sdk::detail
