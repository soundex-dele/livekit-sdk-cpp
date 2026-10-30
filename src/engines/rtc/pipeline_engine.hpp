#pragma once
#include "livekit/rtc/stream_pipeline.hpp"
#include <functional>
#include <deque>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace livekit::sdk::rtc::engine {

// Internal coordinator, called with the shared room operation lock held.
// Media workers only report failures; the next control operation reaps graphs.
// Registry locking is short and is never held while stopping graph workers.
class RtcPipelineEngine {
 public:
  explicit RtcPipelineEngine(RoomEngine&);
  ~RtcPipelineEngine();
  RtcPipelineEngine(const RtcPipelineEngine&) = delete;
  RtcPipelineEngine& operator=(const RtcPipelineEngine&) = delete;

  Result<TrackId> publish(TrackDescriptor,
                          std::shared_ptr<graphflow::core::GraphNode> feedback_sink = {});
  Result<TrackId> subscribe(TrackDescriptor, std::shared_ptr<graphflow::core::GraphNode> consumer);
  // Factories run on the calling thread after endpoint confirmation. They create
  // capture/codec/recording/playout nodes in the endpoint's own GraphEngine.
  // Internal failure cleanup runs before the operation returns,
  // including cancellation and teardown. It must not throw or reenter this engine.
  using FailureCleanup = std::function<void(std::exception_ptr)>;
  Result<TrackId> publish_graph(TrackDescriptor, PublishGraphFactory, FailureCleanup = {});
  Result<TrackId> subscribe_graph(TrackDescriptor, SubscribeGraphFactory, FailureCleanup = {});
  Result<void> unpublish(const TrackId&);
  Result<void> unsubscribe(const TrackId&);
  WriteStatus write(const TrackId&, EncodedFrame);
  TrackSnapshot publication(const TrackId&) const;
  SenderFeedback feedback(const TrackId&) const;

  struct GraphFailure {
    TrackId id;
    bool publication;
    std::string message;
    std::shared_ptr<const FailureInfo> failure;
  };

  void set_failure_handler(std::function<void(GraphFailure)> handler);
  Result<void> clear();
  void reap_failed();

 private:
  Result<void> remove_graph(const TrackId&, bool publication);
  template <class Endpoint>
  GraphStopped observer(const std::shared_ptr<Endpoint>&);

  struct Reports {
    std::mutex mutex;
    std::map<std::shared_ptr<PublishedTrack>, std::exception_ptr> publications;
    std::map<std::shared_ptr<SubscribedTrack>, std::exception_ptr> subscriptions;
    std::function<void(GraphFailure)> handler;
  };
  RoomEngine* room_;
  std::shared_ptr<Reports> reports_ = std::make_shared<Reports>();
  mutable std::mutex mutex_;
  std::map<TrackId, std::shared_ptr<PublishPipeline>> publications_;
  std::map<TrackId, std::shared_ptr<SubscribePipeline>> subscriptions_;
};

}  // namespace livekit::sdk::rtc::engine
