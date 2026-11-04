#pragma once
#include <memory>

namespace livekit::sdk {
struct VideoFrame;

namespace media {
// Optional feedback carried by a received encoded frame. External decoders
// report the actual I420 output once, before presentation scheduling. Copies
// share the same completion; late/duplicate reports after decoder release or
// track rebinding are ignored. No native codec types cross the media boundary.
// Call from the decoder thread, without reentering room lifecycle operations.
class VideoDecodeCompletion {
 public:
  virtual ~VideoDecodeCompletion() = default;
  virtual void decoded(std::shared_ptr<const VideoFrame> frame) = 0;
};
}  // namespace media
}  // namespace livekit::sdk
