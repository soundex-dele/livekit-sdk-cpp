#pragma once
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/stream_pipeline.hpp"

namespace livekit::sdk::rtc::engine {
// Explicit opt-in to GraphFlow types. Borrowed access shares the engine's track
// registry and serial task-queue contract; it must not outlive the engine.
class RtcGraphApi final {
 public:
  AsyncResult<TrackId> publish(TrackDescriptor,
                               std::shared_ptr<graphflow::core::GraphNode> feedback_sink = {});
  AsyncResult<TrackId> subscribe(TrackDescriptor,
                                 std::shared_ptr<graphflow::core::GraphNode> consumer);
  AsyncResult<TrackId> publish_graph(TrackDescriptor, PublishGraphFactory);
  AsyncResult<TrackId> subscribe_graph(TrackDescriptor, SubscribeGraphFactory);

 private:
  friend class RtcEngine;

  explicit RtcGraphApi(RtcEngine& engine) : engine_(&engine) {}

  RtcEngine* engine_;
};
}  // namespace livekit::sdk::rtc::engine
