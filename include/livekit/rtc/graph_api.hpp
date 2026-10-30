#pragma once
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/stream_pipeline.hpp"

namespace livekit::sdk::rtc::engine {
// Explicit opt-in to GraphFlow types. Borrowed access shares the engine's track
// registry and asynchronous completion contract; it must not outlive the engine.
class RtcGraphApi final {
 public:
  [[nodiscard]] ErrorCode publish(TrackDescriptor,
                                  std::shared_ptr<graphflow::core::GraphNode> feedback_sink,
                                  Completion<TrackId> completion);
  [[nodiscard]] ErrorCode subscribe(TrackDescriptor,
                                    std::shared_ptr<graphflow::core::GraphNode> consumer,
                                    Completion<TrackId> completion);
  [[nodiscard]] ErrorCode publish_graph(TrackDescriptor, PublishGraphFactory,
                                        Completion<TrackId> completion);
  [[nodiscard]] ErrorCode subscribe_graph(TrackDescriptor, SubscribeGraphFactory,
                                          Completion<TrackId> completion);

 private:
  friend class RtcEngine;

  explicit RtcGraphApi(RtcEngine& engine) : engine_(&engine) {}

  RtcEngine* engine_;
};
}  // namespace livekit::sdk::rtc::engine
