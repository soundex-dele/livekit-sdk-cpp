#pragma once
#include "api/peer_connection_interface.h"
#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace livekit::sdk::rtc::detail {

// These counters must remain zero in encoded-only sessions. They detect an
// accidental PCM path without silently instantiating a real codec.
struct CodecBoundaryCounters {
  std::atomic<uint64_t> pcm_encode_calls{0}, pcm_decode_calls{0};
  std::atomic<uint64_t> unexpected_video_decode_calls{0};
};

void install_transport_audio_codecs(webrtc::PeerConnectionFactoryDependencies& dependencies,
                                    std::shared_ptr<CodecBoundaryCounters> counters);

// Keep repair codecs alongside the selected media codec, never by themselves.
std::vector<webrtc::RtpCodecCapability> select_transport_codecs(
    const std::vector<webrtc::RtpCodecCapability>& capabilities, const std::string& name,
    const std::map<std::string, std::string>& fmtp, bool enable_flexfec);

}  // namespace livekit::sdk::rtc::detail
