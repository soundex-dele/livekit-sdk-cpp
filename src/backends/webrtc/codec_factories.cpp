#include "backends/webrtc/codec_factories.hpp"
#include "backends/webrtc/graph_video_encoder.hpp"
#include "backends/webrtc/graph_video_decoder.hpp"
#include "api/make_ref_counted.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "modules/video_coding/svc/scalability_mode_util.h"
#include <algorithm>
#include <limits>

namespace livekit::sdk::detail {
namespace {
using namespace webrtc;
using AudioEncoderNode = CodecNode<AudioEncoder>;
using AudioDecoderNode = CodecNode<AudioDecoder>;

class AudioEncoderAdapter final : public AudioEncoder {
 public:
  explicit AudioEncoderAdapter(std::shared_ptr<AudioEncoderNode> node) : node_(std::move(node)) { node_->start(); }
  int SampleRateHz() const override { return b().SampleRateHz(); }
  size_t NumChannels() const override { return b().NumChannels(); }
  int RtpTimestampRateHz() const override { return b().RtpTimestampRateHz(); }
  size_t Num10MsFramesInNextPacket() const override { return b().Num10MsFramesInNextPacket(); }
  size_t Max10MsFramesInAPacket() const override { return b().Max10MsFramesInAPacket(); }
  int GetTargetBitrate() const override { return b().GetTargetBitrate(); }
  void Reset() override { ++node_->generation; b().Reset(); }
  bool SetFec(bool v) override { return b().SetFec(v); }
  bool SetDtx(bool v) override { return b().SetDtx(v); }
  bool GetDtx() const override { return b().GetDtx(); }
  bool SetApplication(Application v) override { return b().SetApplication(v); }
  void SetMaxPlaybackRate(int v) override { b().SetMaxPlaybackRate(v); }
  void SetTargetBitrate(int v) override { b().SetTargetBitrate(v); }
  bool EnableAudioNetworkAdaptor(const std::string& config, RtcEventLog* log) override { return b().EnableAudioNetworkAdaptor(config, log); }
  void DisableAudioNetworkAdaptor() override { b().DisableAudioNetworkAdaptor(); }
  void OnReceivedUplinkPacketLossFraction(float v) override { b().OnReceivedUplinkPacketLossFraction(v); }
  void OnReceivedUplinkRecoverablePacketLossFraction(float v) override { b().OnReceivedUplinkRecoverablePacketLossFraction(v); }
  void OnReceivedTargetAudioBitrate(int v) override { b().OnReceivedTargetAudioBitrate(v); }
  void OnReceivedUplinkBandwidth(int v, std::optional<int64_t> period) override { b().OnReceivedUplinkBandwidth(v, period); }
  void OnReceivedUplinkAllocation(BitrateAllocationUpdate v) override { b().OnReceivedUplinkAllocation(v); }
  void OnReceivedRtt(int v) override { b().OnReceivedRtt(v); }
  void OnReceivedOverhead(size_t v) override { b().OnReceivedOverhead(v); }
  void SetReceiverFrameLengthRange(int min, int max) override { b().SetReceiverFrameLengthRange(min, max); }
  ANAStats GetANAStats() const override { return b().GetANAStats(); }
  std::optional<std::pair<TimeDelta, TimeDelta>> GetFrameLengthRange() const override { return b().GetFrameLengthRange(); }
  std::optional<std::pair<DataRate, DataRate>> GetBitrateRange() const override { return b().GetBitrateRange(); }
 protected:
  EncodedInfo EncodeImpl(uint32_t timestamp, ArrayView<const int16_t> audio, Buffer* encoded) override {
    auto result = node_->process([&](auto& codec) { return codec.Encode(timestamp, audio, encoded); });
    if (result.encoded_bytes) ++node_->outputs; return result;
  }
 private:
  AudioEncoder& b() const { return node_->backend(); }
  std::shared_ptr<AudioEncoderNode> node_;
};

class AudioDecoderAdapter final : public AudioDecoder {
 public:
  explicit AudioDecoderAdapter(std::shared_ptr<AudioDecoderNode> node) : node_(std::move(node)) { node_->start(); }
  class Frame final : public EncodedAudioFrame {
   public:
    Frame(std::shared_ptr<AudioDecoderNode> node, std::unique_ptr<EncodedAudioFrame> frame)
        : node_(std::move(node)), frame_(std::move(frame)) {}
    size_t Duration() const override { return frame_->Duration(); }
    bool IsDtxPacket() const override { return frame_->IsDtxPacket(); }
    std::optional<DecodeResult> Decode(ArrayView<int16_t> output) const override {
      auto result = node_->process([&](auto&) { return frame_->Decode(output); });
      if (result) ++node_->outputs; else ++node_->errors; return result;
    }
   private:
    // Destroy the parsed frame before its backend. NetEq normally also guarantees this.
    std::shared_ptr<AudioDecoderNode> node_;
    std::unique_ptr<EncodedAudioFrame> frame_;
  };
  std::vector<ParseResult> ParsePayload(Buffer&& payload, uint32_t timestamp) override {
    auto result = b().ParsePayload(std::move(payload), timestamp);
    for (auto& item : result) item.frame = std::make_unique<Frame>(node_, std::move(item.frame));
    return result;
  }
  bool HasDecodePlc() const override { return b().HasDecodePlc(); }
  size_t DecodePlc(size_t frames, int16_t* output) override {
    return node_->process([&](auto& codec) { return codec.DecodePlc(frames, output); });
  }
  void GeneratePlc(size_t samples, BufferT<int16_t>* output) override {
    node_->process([&](auto& codec) { codec.GeneratePlc(samples, output); });
  }
  void Reset() override { ++node_->generation; b().Reset(); }
  int ErrorCode() override { return b().ErrorCode(); }
  int PacketDuration(const uint8_t* data, size_t size) const override { return b().PacketDuration(data, size); }
  int PacketDurationRedundant(const uint8_t* data, size_t size) const override { return b().PacketDurationRedundant(data, size); }
  bool PacketHasFec(const uint8_t* data, size_t size) const override { return b().PacketHasFec(data, size); }
  int SampleRateHz() const override { return b().SampleRateHz(); }
  size_t Channels() const override { return b().Channels(); }
 protected:
  int DecodeInternal(const uint8_t* data, size_t size, int rate, int16_t* output, SpeechType* speech) override {
    return node_->process([&](auto& codec) { return codec.Decode(data, size, rate, std::numeric_limits<size_t>::max(), output, speech); });
  }
  int DecodeRedundantInternal(const uint8_t* data, size_t size, int rate, int16_t* output, SpeechType* speech) override {
    return node_->process([&](auto& codec) { return codec.DecodeRedundant(data, size, rate, std::numeric_limits<size_t>::max(), output, speech); });
  }
 private:
  AudioDecoder& b() const { return node_->backend(); }
  std::shared_ptr<AudioDecoderNode> node_;
};

class GraphVideoEncoderFactory final : public VideoEncoderFactory {
 public:
  GraphVideoEncoderFactory(std::shared_ptr<RtcGraph> graph, std::shared_ptr<VideoEncoderFactory> backend)
      : graph_(std::move(graph)), backend_(std::move(backend)) {}
  std::vector<SdpVideoFormat> GetSupportedFormats() const override { return single_spatial_layer(backend_->GetSupportedFormats()); }
  std::vector<SdpVideoFormat> GetImplementations() const override { return single_spatial_layer(backend_->GetImplementations()); }
  CodecSupport QueryCodecSupport(const SdpVideoFormat& format, std::optional<std::string> mode) const override {
    if (mode) {
      auto parsed = ScalabilityModeFromString(*mode);
      if (!parsed || ScalabilityModeToNumSpatialLayers(*parsed) > 1) return {false, false};
    }
    return backend_->QueryCodecSupport(format, mode);
  }
  std::unique_ptr<EncoderSelectorInterface> GetEncoderSelector() const override { return backend_->GetEncoderSelector(); }
  std::unique_ptr<VideoEncoder> Create(const Environment& env, const SdpVideoFormat& format) override {
    if (!backend_->QueryCodecSupport(format, std::nullopt).is_supported) return nullptr;
    try { return create_passthrough_video_encoder(env, format, graph_,
        [backend = backend_](const Environment& env, const SdpVideoFormat& format) -> std::unique_ptr<VideoEncoder> {
          return backend->Create(env, format);
        }); }
    catch (const std::exception&) { return nullptr; }
  }
 private:
  static std::vector<SdpVideoFormat> single_spatial_layer(std::vector<SdpVideoFormat> formats) {
    for (auto& format : formats) {
      auto& modes = format.scalability_modes;
      modes.erase(std::remove_if(modes.begin(), modes.end(), [](auto mode) {
        return ScalabilityModeToNumSpatialLayers(mode) > 1;
      }), modes.end());
    }
    return formats;
  }
  std::shared_ptr<RtcGraph> graph_;
  std::shared_ptr<VideoEncoderFactory> backend_;
};
class GraphVideoDecoderFactory final : public VideoDecoderFactory {
 public:
  GraphVideoDecoderFactory(std::shared_ptr<RtcGraph> graph, std::shared_ptr<VideoDecoderFactory> backend)
      : graph_(std::move(graph)), backend_(std::move(backend)) {}
  std::vector<SdpVideoFormat> GetSupportedFormats() const override { return backend_->GetSupportedFormats(); }
  CodecSupport QueryCodecSupport(const SdpVideoFormat& format, bool scaling) const override {
    return backend_->QueryCodecSupport(format, scaling);
  }
  std::unique_ptr<VideoDecoder> Create(const Environment& env, const SdpVideoFormat& format) override {
    auto codec = backend_->Create(env, format); if (!codec) return nullptr;
    try { return create_graph_video_decoder(graph_, format.name, backend_, std::move(codec)); }
    catch (const std::exception&) { return nullptr; }
  }
 private:
  std::shared_ptr<RtcGraph> graph_;
  std::shared_ptr<VideoDecoderFactory> backend_;
};
class GraphAudioEncoderFactory : public AudioEncoderFactory {
 public:
  GraphAudioEncoderFactory(std::shared_ptr<RtcGraph> graph, scoped_refptr<AudioEncoderFactory> backend)
      : graph_(std::move(graph)), backend_(std::move(backend)) {}
  std::vector<AudioCodecSpec> GetSupportedEncoders() override { return backend_->GetSupportedEncoders(); }
  std::optional<AudioCodecInfo> QueryAudioEncoder(const SdpAudioFormat& format) override { return backend_->QueryAudioEncoder(format); }
  std::unique_ptr<AudioEncoder> Create(const Environment& env, const SdpAudioFormat& format, Options options) override {
    auto codec = backend_->Create(env, format, options); if (!codec) return nullptr;
    try { return std::make_unique<AudioEncoderAdapter>(graph_->add(std::make_shared<AudioEncoderNode>(
        "AudioEncoderNode", format.name, std::make_shared<scoped_refptr<AudioEncoderFactory>>(backend_), std::move(codec)))); }
    catch (const std::exception&) { return nullptr; }
  }
 private:
  std::shared_ptr<RtcGraph> graph_;
  scoped_refptr<AudioEncoderFactory> backend_;
};
class GraphAudioDecoderFactory : public AudioDecoderFactory {
 public:
  GraphAudioDecoderFactory(std::shared_ptr<RtcGraph> graph, scoped_refptr<AudioDecoderFactory> backend)
      : graph_(std::move(graph)), backend_(std::move(backend)) {}
  std::vector<AudioCodecSpec> GetSupportedDecoders() override {
    auto formats = backend_->GetSupportedDecoders();
    for (auto& codec : formats) {
      auto name = codec.format.name;
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (name != "opus" || codec.format.num_channels != 2) continue;
      auto stereo = codec.format; stereo.parameters["stereo"] = "1";
      if (!backend_->IsSupportedDecoder(stereo)) continue;
      // Stock Opus can decode stereo but advertises mono by default. Advertise
      // our actual receive capability so SDP configures NetEq for two channels;
      // setting only AddTrack.stereo on the publisher cannot prevent downmix.
      codec.format = std::move(stereo); codec.info.num_channels = 2;
    }
    return formats;
  }
  bool IsSupportedDecoder(const SdpAudioFormat& format) override { return backend_->IsSupportedDecoder(format); }
  std::unique_ptr<AudioDecoder> Create(const Environment& env, const SdpAudioFormat& format, std::optional<AudioCodecPairId> id) override {
    auto codec = backend_->Create(env, format, id); if (!codec) return nullptr;
    try { return std::make_unique<AudioDecoderAdapter>(graph_->add(std::make_shared<AudioDecoderNode>(
        "AudioDecoderNode", format.name, std::make_shared<scoped_refptr<AudioDecoderFactory>>(backend_), std::move(codec)))); }
    catch (const std::exception&) { return nullptr; }
  }
 private:
  std::shared_ptr<RtcGraph> graph_;
  scoped_refptr<AudioDecoderFactory> backend_;
};
}

CodecBackends builtin_codecs() {
  return {CreateBuiltinVideoEncoderFactory(), CreateBuiltinVideoDecoderFactory(),
          CreateBuiltinAudioEncoderFactory(), CreateBuiltinAudioDecoderFactory()};
}
void install_graph_codecs(webrtc::PeerConnectionFactoryDependencies& deps,
                          std::shared_ptr<RtcGraph> graph, CodecBackends backends) {
  if (!graph || !backends.video_encoder || !backends.video_decoder || !backends.audio_encoder || !backends.audio_decoder)
    throw std::invalid_argument("media_graph: missing codec backend");
  deps.video_encoder_factory = std::make_unique<GraphVideoEncoderFactory>(graph, std::move(backends.video_encoder));
  deps.video_decoder_factory = std::make_unique<GraphVideoDecoderFactory>(graph, std::move(backends.video_decoder));
  deps.audio_encoder_factory = webrtc::make_ref_counted<GraphAudioEncoderFactory>(graph, std::move(backends.audio_encoder));
  deps.audio_decoder_factory = webrtc::make_ref_counted<GraphAudioDecoderFactory>(graph, std::move(backends.audio_decoder));
}
}
