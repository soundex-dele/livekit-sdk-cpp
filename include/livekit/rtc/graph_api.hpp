#pragma once
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/stream_pipeline.hpp"

namespace livekit::sdk::rtc::engine {
// Optional custom topology. Results use per-call completions, like media controls.
class RtcGraphApi final {
 public:
  void publish(TrackDescriptor, std::shared_ptr<graphflow::core::GraphNode> feedback_sink,
               Completion<MediaPublication> = {});
  void subscribe(TrackDescriptor, std::shared_ptr<graphflow::core::GraphNode> consumer,
                 Completion<MediaSubscription> = {});
  void publish_graph(TrackDescriptor, PublishGraphFactory, Completion<MediaPublication> = {});
  void subscribe_graph(TrackDescriptor, SubscribeGraphFactory, Completion<MediaSubscription> = {});

 private:
  friend class RtcEngine;

  explicit RtcGraphApi(RtcEngine& engine) : engine_(&engine) {}

  RtcEngine* engine_;
};
}  // namespace livekit::sdk::rtc::engine
