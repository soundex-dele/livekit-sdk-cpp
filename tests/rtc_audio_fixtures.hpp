#pragma once
#include "livekit/rtc/audio_nodes.hpp"
#include <cmath>
#include <numbers>

namespace rtc_audio_test {
using namespace livekit::sdk::rtc;

inline TrackDescriptor descriptor(std::string id, uint16_t channels = 1) {
  TrackDescriptor d;
  d.id = d.name = std::move(id);
  d.kind = MediaKind::audio;
  d.format.codec = Codec::opus;
  d.format.clock_rate = 48000;
  d.format.channels = channels;
  d.encodings.front().max_bitrate_bps = 128000;
  return d;
}

inline nodes::AudioFrameEvent tone(Epoch epoch, uint64_t timestamp, uint32_t rate = 44100,
                                   uint16_t channels = 1, bool floating = true,
                                   double frequency = 440) {
  auto pcm = std::make_shared<PcmAudioFrame>();
  pcm->sample_rate = rate;
  pcm->channels = channels;
  pcm->format = floating ? PcmFormat::f32 : PcmFormat::s16;
  for (size_t i = 0; i < rate / 100; ++i)
    for (size_t ch = 0; ch < channels; ++ch) {
      const double value = .3 * std::sin(2 * std::numbers::pi * frequency * (1 + .5 * ch) *
                                         (double(timestamp) / 48000 + double(i) / rate));
      if (floating)
        pcm->float_samples.push_back(float(value));
      else
        pcm->samples.push_back(int16_t(value * 32767));
    }
  nodes::AudioFrameEvent frame;
  frame.pcm = std::move(pcm);
  frame.timestamp = timestamp;
  frame.epoch = epoch;
  return frame;
}

inline double tone_power(const PcmAudioFrame& pcm, size_t channel, double frequency) {
  double real = 0, imaginary = 0;
  const auto count = pcm.samples_per_channel();
  for (size_t i = 0; i < count; ++i) {
    const double angle = 2 * std::numbers::pi * frequency * i / pcm.sample_rate;
    const double sample = pcm.samples[i * pcm.channels + channel];
    real += sample * std::cos(angle);
    imaginary += sample * std::sin(angle);
  }
  return (real * real + imaginary * imaginary) / (count * count);
}
}  // namespace rtc_audio_test
