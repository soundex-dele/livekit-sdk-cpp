#pragma once
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/stream_pipeline.hpp"

namespace livekit::sdk::rtc::engine {
// Explicit opt-in to GraphFlow types. Borrowed access shares the engine's track
// registry and asynchronous observer contract; it must not outlive the engine.
class RtcGraphApi final {
 public:
  ErrorCode publish(TrackDescriptor,
                    std::shared_ptr<graphflow::core::GraphNode> feedback_sink = {});
  ErrorCode subscribe(TrackDescriptor, std::shared_ptr<graphflow::core::GraphNode> consumer);
  ErrorCode publish_graph(TrackDescriptor, PublishGraphFactory);
  ErrorCode subscribe_graph(TrackDescriptor, SubscribeGraphFactory);

 private:
  friend class RtcEngine;

  explicit RtcGraphApi(RtcEngine& engine) : engine_(&engine) {}

  RtcEngine* engine_;
};
}  // namespace livekit::sdk::rtc::engine
