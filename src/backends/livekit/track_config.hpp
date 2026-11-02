#pragma once
#include "livekit/client/types.hpp"

namespace livekit::sdk::client::detail {
struct TrackConfig {
  TrackId id;
  std::string name, stream_group;
  MediaKind kind = MediaKind::video;
  CodecFormat format;
  QueueLimits queue;
  std::vector<EncodingDescriptor> encodings{{"", 2000000, true}};
  bool muted = false, follow_subscriber_demand = false;
  PublicationSource source = PublicationSource::unknown;
};
}  // namespace livekit::sdk::client::detail
