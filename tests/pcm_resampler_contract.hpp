#pragma once
#include "backends/webrtc/pcm_resampler.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

inline void pcm_resampler_contract() {
  auto require = [](bool ok) { if (!ok) throw std::runtime_error("PCM resampler contract failed"); };
  for (const int rate : {8000,16000,24000,32000,44100,48000,96000}) {
    livekit::sdk::detail::PcmResampler resampler(rate, 2);
    int crossings = 0, previous = 0;
    std::vector<int16_t> input(rate / 100 * 2);
    for (int block = 0; block < 102; ++block) {
      for (int i = 0; i < rate / 100; ++i) {
        input[i * 2] = static_cast<int16_t>(6000 * std::sin(2 * std::numbers::pi * 1000 * (block * (rate / 100) + i) / rate));
        input[i * 2 + 1] = 0;
      }
      auto out = resampler.process(input); require(out.size() == 960);
      for (size_t i = 0; i < 480; ++i) {
        require(out[i * 2 + 1] == 0);
        if (block >= 2 && previous < 0 && out[i * 2] >= 0) ++crossings;
        previous = out[i * 2];
      }
    }
    require(std::abs(crossings - 1000) <= 2);
    resampler.reset(); std::fill(input.begin(), input.end(), 0);
    const auto zero = resampler.process(input);
    require(std::all_of(zero.begin(), zero.end(), [](auto sample) { return sample == 0; }));
  }
}
