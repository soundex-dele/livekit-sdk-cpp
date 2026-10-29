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

// Scene-engine building block. One RoomEngine is shared by all track graphs.
// The engine task queue handles completion, node failures and endpoint closure.
// Source/feedback nodes dispatch on their own event-driven workers; no media
// delivery runs on the controller or backend transport thread. Control methods are owner-thread only;
// write() may be called by media producers. Consumers must not reenter graph
// lifecycle methods from inside synchronous GraphEngine delivery.
class RtcPipelineEngine {
 public:
  using Dispatch = std::function<void(std::function<void()>)>;
  RtcPipelineEngine(RoomEngine&, Dispatch);
  ~RtcPipelineEngine();
  RtcPipelineEngine(const RtcPipelineEngine&) = delete;
  RtcPipelineEngine& operator=(const RtcPipelineEngine&) = delete;

  AsyncResult<TrackId> publish(TrackDescriptor,
                               std::shared_ptr<graphflow::core::GraphNode> feedback_sink = {});
  AsyncResult<TrackId> subscribe(TrackDescriptor,
                                 std::shared_ptr<graphflow::core::GraphNode> consumer);
  // Factories run on the engine task queue after endpoint confirmation. They create
  // capture/codec/recording/playout nodes in the endpoint's own GraphEngine.
  // Internal failure cleanup runs before the operation's future becomes ready,
  // including cancellation and teardown. It must not throw or reenter this engine.
  using FailureCleanup = std::function<void(std::exception_ptr)>;
  AsyncResult<TrackId> publish_graph(TrackDescriptor, PublishGraphFactory, FailureCleanup = {});
  AsyncResult<TrackId> subscribe_graph(TrackDescriptor, SubscribeGraphFactory, FailureCleanup = {});
  AsyncResult<void> unpublish(const TrackId&);
  AsyncResult<void> unsubscribe(const TrackId&);
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
  void clear();

 private:
  void require_owner() const;
  void remove_graph(const TrackId&, bool publication);
  void cancel_pending(const TrackId&, bool publication);
  template <class Endpoint>
  GraphStopped observer(const std::shared_ptr<Endpoint>&);
  void stopped_graph(const std::shared_ptr<PublishedTrack>&, std::exception_ptr);
  void stopped_graph(const std::shared_ptr<SubscribedTrack>&, std::exception_ptr);

  struct Pending {
    TrackId id;
    bool publication;
    bool settled = false;
    std::function<void(std::exception_ptr)> cancel;
  };

  RoomEngine* room_;
  Dispatch dispatch_;
  std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
  const std::thread::id owner_ = std::this_thread::get_id();
  mutable std::mutex mutex_;
  std::map<TrackId, std::shared_ptr<PublishPipeline>> publications_;
  std::map<TrackId, std::shared_ptr<SubscribePipeline>> subscriptions_;
  std::vector<std::shared_ptr<Pending>> pending_;
  std::function<void(GraphFailure)> failure_handler_;
};

}  // namespace livekit::sdk::rtc::engine
