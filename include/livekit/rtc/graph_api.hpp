#pragma once
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/stream_pipeline.hpp"

namespace livekit::sdk::rtc::engine {
// Optional custom topology. Completion uses RtcCallbacks, like media controls.
class RtcGraphApi final {
 public:
  void publish(TrackDescriptor, std::shared_ptr<graphflow::core::GraphNode> feedback_sink);
  void subscribe(TrackDescriptor, std::shared_ptr<graphflow::core::GraphNode> consumer);
  void publish_graph(TrackDescriptor, PublishGraphFactory);
  void subscribe_graph(TrackDescriptor, SubscribeGraphFactory);

 private:
  friend class RtcEngine;

  explicit RtcGraphApi(RtcEngine& engine) : engine_(&engine) {}

  RtcEngine* engine_;
};
}  // namespace livekit::sdk::rtc::engine
