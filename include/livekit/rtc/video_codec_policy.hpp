#pragma once
#include "livekit/error.hpp"
#include <memory>
#include <string>

namespace livekit::sdk::rtc {
// Shared participant-attribute protocol. Put capabilities in the join token to
// avoid treating a new participant as unknown before its first attribute update.
inline constexpr char kVideoDecodeCodecsAttribute[] = "av.video.decode_codecs";
inline constexpr char kVideoH264LatchAttribute[] = "av.video.h264_latched";

// Opt-in RtcEngine policy. Every publisher must participate. H264 decoding and
// encoding are prerequisites; declare H265 only when the local decoder supports it.
// Unknown peers are conservatively treated as H264-only. LiveKit requires
// protocol 15+ and canUpdateOwnMetadata permission to propagate the sticky latch.
struct RoomVideoCodecPolicy {
  bool can_decode_h265 = false;
};

struct VideoCodecPolicySnapshot {
  bool enabled = false;
  bool h264_latched = false;
  // Local transitions only; this is not an acknowledgement from every publisher.
  bool transitioning = false;
  std::shared_ptr<const FailureInfo> failure;
};
}  // namespace livekit::sdk::rtc
