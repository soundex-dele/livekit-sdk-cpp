#pragma once
#include "api/video_codecs/video_decoder.h"
#include <string>

namespace livekit::sdk::detail {
// Backend-facing lease; keeps codec adapters independent of SDK output targets.
class VideoDecodeTarget {
 public:
  virtual ~VideoDecodeTarget() = default;
  virtual uint64_t generation() const = 0;
  virtual int decode(uint64_t generation, webrtc::VideoDecoder& codec,
                     const webrtc::EncodedImage& image, std::optional<bool> missing,
                     int64_t render_time_ms, const std::string& format) = 0;
  virtual void decoded(uint64_t generation, const webrtc::VideoFrame& frame) = 0;
};
}
