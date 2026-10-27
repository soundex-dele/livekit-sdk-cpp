#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace livekit::sdk {
enum class CapabilitySupport { unknown, unsupported, supported };
enum class ClientFeature {
  signal_resume, full_rejoin, initial_signal_failover, token_provider,
  i420_input, rgba_input, pcm_input, video_pull, audio_pull,
  subscription_permissions, receive_quality, dtmf, transcription, rpc,
  text_stream, byte_stream, data_track, media_e2ee, data_e2ee,
  simulcast, single_peer_connection, encoded_media, rtp_access, control_executor
};
struct SdkCapabilities {
  std::string version;
  std::vector<int> signal_protocols;
  std::map<ClientFeature,CapabilitySupport> features;
  CapabilitySupport support(ClientFeature feature) const {
    const auto found=features.find(feature);return found==features.end()?CapabilitySupport::unknown:found->second;
  }
};
// Reports public RTC API implementation, not server/device availability or
// cross-platform acceptance. Does not initialize WebRTC or open devices/network.
SdkCapabilities sdk_capabilities();
struct ServerCodec {std::string mime_type,fmtp_line;};
struct ServerCapabilities {
  std::string version,region;
  std::optional<int> advertised_protocol;
  // nullopt means no list was advertised, not "no codec is supported".
  std::optional<std::vector<ServerCodec>> enabled_publish_codecs;
  // Only explicit evidence can populate this map. An omitted feature is unknown.
  std::map<ClientFeature,CapabilitySupport> features;
  CapabilitySupport support(ClientFeature feature) const {
    const auto found=features.find(feature);return found==features.end()?CapabilitySupport::unknown:found->second;
  }
};
struct SessionCapabilities {
  uint64_t generation=0;
  bool connected=false;
  std::optional<int> signal_protocol;
  std::optional<bool> subscriber_primary,relay_only,single_peer_connection;
  ServerCapabilities server;
};
}
