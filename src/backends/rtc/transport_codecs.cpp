#include "backends/rtc/transport_codecs.hpp"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/make_ref_counted.h"

namespace livekit::sdk::rtc::detail {
namespace {
bool opus(const webrtc::SdpAudioFormat& format) {
  return (format.name == "opus" || format.name == "OPUS") &&
         format.clockrate_hz == 48000 && format.num_channels == 2;
}
webrtc::AudioCodecInfo opus_info() {
  webrtc::AudioCodecInfo info(48000, 2, 32000, 6000, 510000);
  info.allow_comfort_noise = false;
  return info;
}
std::vector<webrtc::AudioCodecSpec> formats() {
  return {{webrtc::SdpAudioFormat("opus", 48000, 2, {{"minptime", "10"}}), opus_info()}};
}

// Native audio streams require codec metadata to negotiate payload type and
// clock rate. External frames enter ChannelSend after this encoder boundary.
// If PCM unexpectedly arrives, it never turns into packets or decoded output.
class AudioEnvelope final : public webrtc::AudioEncoder {
 public:
  explicit AudioEnvelope(std::shared_ptr<CodecBoundaryCounters> counters)
      : counters_(std::move(counters)) {}
  int SampleRateHz() const override { return 48000; }
  size_t NumChannels() const override { return 2; }
  size_t Num10MsFramesInNextPacket() const override { return 2; }
  size_t Max10MsFramesInAPacket() const override { return 2; }
  int GetTargetBitrate() const override { return bitrate_; }
  void OnReceivedTargetAudioBitrate(int rate) override { bitrate_ = rate; }
  void Reset() override {}
  std::optional<std::pair<webrtc::TimeDelta, webrtc::TimeDelta>> GetFrameLengthRange() const override {
    // Native congestion allocation divides packet overhead by this range.
    // Opus access units may aggregate up to 120 ms; no PCM cadence supplies
    // this metadata on the external encoded path.
    return std::pair(webrtc::TimeDelta::Micros(2500), webrtc::TimeDelta::Millis(120));
  }
 protected:
  EncodedInfo EncodeImpl(uint32_t, webrtc::ArrayView<const int16_t>, webrtc::Buffer*) override {
    ++counters_->pcm_encode_calls;
    return {};
  }
 private:
  std::shared_ptr<CodecBoundaryCounters> counters_;
  int bitrate_ = 32000;
};
class AudioBoundary final : public webrtc::AudioDecoder {
 public:
  explicit AudioBoundary(std::shared_ptr<CodecBoundaryCounters> counters)
      : counters_(std::move(counters)) {}
  int SampleRateHz() const override { return 48000; }
  size_t Channels() const override { return 2; }
  void Reset() override {}
 protected:
  int DecodeInternal(const uint8_t*, size_t, int, int16_t*, SpeechType*) override {
    ++counters_->pcm_decode_calls;
    return -1;
  }
 private:
  std::shared_ptr<CodecBoundaryCounters> counters_;
};
class AudioEncoderFactory : public webrtc::AudioEncoderFactory {
 public:
  explicit AudioEncoderFactory(std::shared_ptr<CodecBoundaryCounters> counters)
      : counters_(std::move(counters)) {}
  std::vector<webrtc::AudioCodecSpec> GetSupportedEncoders() override { return formats(); }
  std::optional<webrtc::AudioCodecInfo> QueryAudioEncoder(const webrtc::SdpAudioFormat& format) override {
    return opus(format) ? std::optional(opus_info()) : std::nullopt;
  }
  std::unique_ptr<webrtc::AudioEncoder> Create(const webrtc::Environment&,
      const webrtc::SdpAudioFormat& format, Options) override {
    return opus(format) ? std::make_unique<AudioEnvelope>(counters_) : nullptr;
  }
 private:
  std::shared_ptr<CodecBoundaryCounters> counters_;
};
class AudioDecoderFactory : public webrtc::AudioDecoderFactory {
 public:
  explicit AudioDecoderFactory(std::shared_ptr<CodecBoundaryCounters> counters)
      : counters_(std::move(counters)) {}
  std::vector<webrtc::AudioCodecSpec> GetSupportedDecoders() override { return formats(); }
  bool IsSupportedDecoder(const webrtc::SdpAudioFormat& format) override { return opus(format); }
  std::unique_ptr<webrtc::AudioDecoder> Create(const webrtc::Environment&,
      const webrtc::SdpAudioFormat& format, std::optional<webrtc::AudioCodecPairId>) override {
    return opus(format) ? std::make_unique<AudioBoundary>(counters_) : nullptr;
  }
 private:
  std::shared_ptr<CodecBoundaryCounters> counters_;
};
} // namespace

void install_transport_audio_codecs(webrtc::PeerConnectionFactoryDependencies& deps,
                                   std::shared_ptr<CodecBoundaryCounters> counters) {
  deps.audio_encoder_factory = webrtc::make_ref_counted<AudioEncoderFactory>(counters);
  deps.audio_decoder_factory = webrtc::make_ref_counted<AudioDecoderFactory>(std::move(counters));
}
} // namespace livekit::sdk::rtc::detail
