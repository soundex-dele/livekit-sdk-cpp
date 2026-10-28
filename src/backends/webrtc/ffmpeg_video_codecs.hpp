#pragma once
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
#include <memory>

namespace livekit::sdk::detail {
// Optional software backends, single-layer 8-bit 4:2:0 only. Actual FFmpeg
// encoder/decoder availability controls advertisement; no hardware claim.
std::shared_ptr<webrtc::VideoEncoderFactory> with_ffmpeg_encoders(std::shared_ptr<webrtc::VideoEncoderFactory> fallback);
std::shared_ptr<webrtc::VideoDecoderFactory> with_ffmpeg_decoders(std::shared_ptr<webrtc::VideoDecoderFactory> fallback);
}
