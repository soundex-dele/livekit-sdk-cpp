#pragma once
#include "livekit/compat/span.hpp"
#include "common_audio/resampler/include/push_resampler.h"
#include <cstdint>
#include <memory>
#include "livekit/result.hpp"
#include <vector>

namespace livekit::sdk::detail {
// Per-publication streaming filter. Caller supplies exactly 10ms of PCM16.
class PcmResampler {
 public:
  PcmResampler(int rate, size_t channels) : input_samples_(rate / 100), channels_(channels) {
    if (rate < 8000 || rate > 96000 || rate % 100 || (channels != 1 && channels != 2)) {
      failure_ = make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                              "Invalid PCM resampler format");
      return;
    }
    output_.resize(480 * channels);
    reset();
  }

  Result<void> status() const {
    return failure_ ? Result<void>::failure(failure_) : Result<void>::success();
  }

  void reset() {
    if (failure_) {
      return;
    }
    resampler_ = std::make_unique<webrtc::PushResampler<int16_t>>(input_samples_, 480, channels_);
  }

  Result<livekit::compat::span<const int16_t>> process(livekit::compat::span<const int16_t> input) {
    if (failure_) {
      return Result<livekit::compat::span<const int16_t>>::failure(failure_);
    }
    if (input.size() != input_samples_ * channels_) {
      return Result<livekit::compat::span<const int16_t>>::failure(make_failure(
          ErrorDomain::media, ErrorCode::invalid_argument, "PCM resampler requires a 10ms block"));
    }
    const auto count = resampler_->Resample(
        webrtc::InterleavedView<const int16_t>(input.data(), input_samples_, channels_),
        webrtc::InterleavedView<int16_t>(output_.data(), 480, channels_));
    if (count != static_cast<int>(output_.size())) {
      return Result<livekit::compat::span<const int16_t>>::failure(
          make_failure(ErrorDomain::media, ErrorCode::processing_failed,
                       "PCM resampling produced an invalid block"));
    }
    return Result<livekit::compat::span<const int16_t>>::success(output_);
  }

 private:
  Failure failure_;
  size_t input_samples_, channels_;
  std::vector<int16_t> output_;
  std::unique_ptr<webrtc::PushResampler<int16_t>> resampler_;
};
}  // namespace livekit::sdk::detail
