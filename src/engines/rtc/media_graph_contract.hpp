#pragma once
#include "livekit/rtc/media_config.hpp"
#include "livekit/rtc/track_graph.hpp"

namespace livekit::sdk::rtc::engine {
// Type-erased ownership keeps core RTC usable without codecs/device libraries.
class MediaResources {
 public:
  virtual ~MediaResources() = default;
};

struct PreparedMediaGraph {
  TrackGraphSpec graph;
  std::shared_ptr<MediaTrack> track;
};

// Builds fresh nodes against a single endpoint port. Never starts a graph,
// registers a track or issues room commands. Called on the control sequence.
class MediaGraphBuilder {
 public:
  virtual ~MediaGraphBuilder() = default;
  virtual PreparedMediaGraph build_publish(MediaPublishSpec,
                                           std::shared_ptr<nodes::RtcPublishNode>) = 0;
  virtual PreparedMediaGraph build_subscribe(MediaSubscribeSpec,
                                             std::shared_ptr<nodes::RtcSubscribeNode>) = 0;
  virtual std::shared_ptr<SharedMediaInput> create_shared_input(TrackDescriptor, MediaInput) = 0;
};
}  // namespace livekit::sdk::rtc::engine
