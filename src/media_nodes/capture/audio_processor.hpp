#pragma once
#include "livekit/rtc/audio_processing.hpp"
#include "livekit/rtc/audio_nodes.hpp"
#include "backends/webrtc/pcm_resampler.hpp"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
#include <cmath>

namespace livekit::sdk::rtc::detail {
// Used only by a capture/input worker. Device callbacks only write the bounded
// reference history; all APM work and allocation happen here.
class AudioProcessor final {
 public:
  AudioProcessor(AudioProcessingOptions options, std::shared_ptr<AudioRenderReference> reference)
      : options_(options), reference_(std::move(reference)) {
    if (options.device_delay.count() < 0 || options.device_delay > std::chrono::milliseconds(500)) {
      throw std::invalid_argument("Invalid audio processing device delay");
    }
    if (options.enabled && options.echo_cancellation && !reference_) {
      throw std::invalid_argument("Echo cancellation requires a render reference");
    }
  }

  void process(nodes::AudioFrameEvent& event) {
    if (!options_.enabled) {
      return;
    }
    const auto& input = *event.pcm;
    if (input.samples_per_channel() != input.sample_rate / 100 ||
        (input.channels != 1 && input.channels != 2)) {
      throw std::invalid_argument("Audio processing requires 10 ms mono/stereo PCM");
    }
    if (!apm_ || epoch_ != event.epoch || input.sample_rate != rate_ ||
        input.channels != channels_ || event.discontinuity) {
      webrtc::AudioProcessing::Config config;
      config.echo_canceller.enabled = options_.echo_cancellation;
      config.noise_suppression.enabled = options_.noise_suppression;
      config.high_pass_filter.enabled = true;
      config.gain_controller2.enabled = options_.automatic_gain;
      config.gain_controller2.adaptive_digital.enabled = options_.automatic_gain;
      apm_ = webrtc::BuiltinAudioProcessingBuilder(config).Build(webrtc::CreateEnvironment());
      if (!apm_) {
        throw std::runtime_error("Unable to create audio processor");
      }
      rate_ = input.sample_rate;
      channels_ = input.channels;
      epoch_ = event.epoch;
      resampler_ = std::make_unique<livekit::sdk::detail::PcmResampler>(rate_, channels_);
    }
    if (reference_) {
      for (const auto& block : reference_->read_since(reference_cursor_)) {
        if (std::chrono::steady_clock::now() - block.rendered > std::chrono::milliseconds(200)) {
          continue;
        }
        std::array<int16_t, 960> reverse;
        const webrtc::StreamConfig stereo(48000, 2);
        if (apm_->ProcessReverseStream(block.stereo.data(), stereo, stereo, reverse.data()) != 0) {
          throw std::runtime_error("Audio reverse processing failed");
        }
      }
    }
    apm_->set_stream_delay_ms(int(options_.device_delay.count()));
    std::vector<int16_t> converted;
    std::span<const int16_t> samples = input.samples;
    if (input.format == PcmFormat::f32) {
      converted.reserve(input.float_samples.size());
      for (auto sample : input.float_samples) {
        if (!std::isfinite(sample)) {
          throw std::invalid_argument("Non-finite capture sample");
        }
        converted.push_back(int16_t(
            std::clamp(std::lround(std::clamp(sample, -1.f, 1.f) * 32768.f), -32768l, 32767l)));
      }
      samples = converted;
    }
    const auto normalized = resampler_->process(samples);
    auto output = std::make_shared<PcmAudioFrame>();
    output->channels = channels_;
    output->samples.resize(480 * channels_);
    const webrtc::StreamConfig stream(48000, channels_);
    if (apm_->ProcessStream(normalized.data(), stream, stream, output->samples.data()) != 0) {
      throw std::runtime_error("Audio capture processing failed");
    }
    event.pcm = std::move(output);
  }

 private:
  AudioProcessingOptions options_;
  std::shared_ptr<AudioRenderReference> reference_;
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm_;
  std::unique_ptr<livekit::sdk::detail::PcmResampler> resampler_;
  uint32_t rate_ = 0;
  uint16_t channels_ = 0;
  Epoch epoch_ = 0;
  uint64_t reference_cursor_ = 0;
};
}  // namespace livekit::sdk::rtc::detail
