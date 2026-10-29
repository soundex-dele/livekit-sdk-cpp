#pragma once
#include "api/peer_connection_interface.h"
#include <atomic>

namespace livekit::sdk::rtc::detail {

// These counters must remain zero in encoded-only sessions. They detect an
// accidental PCM path without silently instantiating a real codec.
struct CodecBoundaryCounters {
  std::atomic<uint64_t> pcm_encode_calls{0}, pcm_decode_calls{0};
  std::atomic<uint64_t> unexpected_video_decode_calls{0};
};
void install_transport_audio_codecs(
    webrtc::PeerConnectionFactoryDependencies& dependencies,
    std::shared_ptr<CodecBoundaryCounters> counters);

} // namespace livekit::sdk::rtc::detail
