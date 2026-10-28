#include "backends/webrtc/ffmpeg_video_codecs.hpp"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/video_codec.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace webrtc;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct DecodedSink : DecodedImageCallback {
  int count = 0; uint32_t timestamp = 0;
  int32_t Decoded(VideoFrame& frame) override {
    auto pixels = frame.video_frame_buffer()->ToI420();
    check(pixels && pixels->width() == 320 && pixels->height() == 180, "decode dimensions");
    check(std::abs(int(pixels->DataY()[100]) - 80) <= 8, "decode pixels");
    timestamp = frame.rtp_timestamp(); ++count; return 0;
  }
};
struct Encoded : EncodedImageCallback {
  VideoDecoder* decoder = nullptr; int count = 0, keys = 0, dropped = 0;
  Result OnEncodedImage(const EncodedImage& image, const CodecSpecificInfo*) override {
    check(image.size() > 0, "empty compressed frame"); ++count;
    if (image._frameType == VideoFrameType::kVideoFrameKey) ++keys;
    check(decoder->Decode(image, int64_t(0)) == 0, "decode failed");
    return Result(Result::OK);
  }
  void OnDroppedFrame(DropReason) override { ++dropped; }
};
int main() {
  try {
    auto encoders = livekit::sdk::detail::with_ffmpeg_encoders(CreateBuiltinVideoEncoderFactory());
    auto decoders = livekit::sdk::detail::with_ffmpeg_decoders(CreateBuiltinVideoDecoderFactory());
    const auto environment = CreateEnvironment();
    for (const auto* name : {"H264", "H265"}) {
      auto formats = encoders->GetSupportedFormats();
      auto format = std::find_if(formats.begin(), formats.end(), [&](const auto& f) { return f.name == name; });
      check(format != formats.end(), "missing FFmpeg encoder");
      check(encoders->QueryCodecSupport(*format, "L1T1").is_supported &&
            !encoders->QueryCodecSupport(*format, "L1T3").is_supported &&
            !encoders->QueryCodecSupport(*format, {}).is_power_efficient, "single software layer capabilities");
      check(decoders->QueryCodecSupport(*format, false).is_supported &&
            !decoders->QueryCodecSupport(*format, true).is_supported, "decoder reference scaling capabilities");
      format->name = std::string(name) == "H264" ? "h264" : "h265";
      auto encoder = encoders->Create(environment, *format);
      auto decoder = decoders->Create(environment, *format);
      check(encoder && decoder, "codec creation failed");
      VideoCodec codec{}; codec.codecType = std::string(name) == "H264" ? kVideoCodecH264 : kVideoCodecH265;
      codec.width = 320; codec.height = 180; codec.maxFramerate = 30; codec.startBitrate = 200;
      check(encoder->InitEncode(&codec, VideoEncoder::Settings(VideoEncoder::Capabilities(false), 1, 1200)) == 0, "init encoder");
      VideoDecoder::Settings settings; settings.set_codec_type(codec.codecType);
      check(decoder->Configure(settings), "init decoder");
      DecodedSink decoded;
      Encoded encoded; encoded.decoder = decoder.get();
      decoder->RegisterDecodeCompleteCallback(&decoded); encoder->RegisterEncodeCompleteCallback(&encoded);
      auto buffer = I420Buffer::Create(320, 180);
      std::memset(buffer->MutableDataY(), 80, buffer->StrideY() * 180);
      std::memset(buffer->MutableDataU(), 100, buffer->StrideU() * 90);
      std::memset(buffer->MutableDataV(), 160, buffer->StrideV() * 90);
      auto frame = VideoFrame::Builder().set_video_frame_buffer(buffer).set_rtp_timestamp(1234).build();
      std::vector<VideoFrameType> types{VideoFrameType::kVideoFrameDelta};
      check(encoder->Encode(frame, &types) == 0 && decoded.count == 1 && decoded.timestamp == 1234, "first IDR/timestamp");
      frame.set_rtp_timestamp(4234);
      check(encoder->Encode(frame, &types) == 0 && decoded.count == 2 && encoded.keys == 1, "delta frame");
      types[0] = VideoFrameType::kVideoFrameKey;
      frame.set_rtp_timestamp(7234);
      check(encoder->Encode(frame, &types) == 0 && encoded.keys == 2, "PLI keyframe request");
      VideoBitrateAllocation rates;
      encoder->SetRates(VideoEncoder::RateControlParameters(rates, 30));
      check(encoder->Encode(frame, &types) == 0 && encoded.count == 3 && encoded.dropped == 1, "zero rate must pause");
      rates.SetBitrate(0, 0, 100000);
      encoder->SetRates(VideoEncoder::RateControlParameters(rates, 15));
      frame.set_rtp_timestamp(10234);
      check(encoder->Encode(frame, &types) == 0 && decoded.count == 4 && decoded.timestamp == 10234, "rate change recovery");
      encoder->Release(); decoder->Release();
      check(encoder->Encode(frame, &types) != 0, "released encoder accepted frame");
      std::cout << name << ": roundtrip, PLI, pause, rate change, release PASS\n";
    }
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
