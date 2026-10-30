#pragma once
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/stream_pipeline.hpp"

namespace livekit::sdk::rtc::engine {
// Explicit opt-in to GraphFlow types. Borrowed access shares the engine's track
// registry and synchronous control contract; it must not outlive the engine.
class RtcGraphApi final {
 public:
  TrackId publish(TrackDescriptor, std::shared_ptr<graphflow::core::GraphNode> feedback_sink = {},
                  ErrorCode* error = nullptr);
  TrackId subscribe(TrackDescriptor, std::shared_ptr<graphflow::core::GraphNode> consumer,
                    ErrorCode* error = nullptr);
  TrackId publish_graph(TrackDescriptor, PublishGraphFactory, ErrorCode* error = nullptr);
  TrackId subscribe_graph(TrackDescriptor, SubscribeGraphFactory, ErrorCode* error = nullptr);

 private:
  friend class RtcEngine;

  explicit RtcGraphApi(RtcEngine& engine) : engine_(&engine) {}

  RtcEngine* engine_;
};
}  // namespace livekit::sdk::rtc::engine
