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
// The engine task queue handles completion notifications and health checks.
// Source/feedback nodes dispatch on their own event-driven workers; no media
// delivery runs on the controller or backend transport thread. Control methods are owner-thread only;
// write() may be called by media producers. Consumers must not reenter graph
// lifecycle methods from inside synchronous GraphEngine delivery.
class RtcPipelineEngine {
 public:
  explicit RtcPipelineEngine(RoomEngine&);
  ~RtcPipelineEngine();
  RtcPipelineEngine(const RtcPipelineEngine&) = delete;
  RtcPipelineEngine& operator=(const RtcPipelineEngine&) = delete;

  std::future<TrackId> publish(TrackDescriptor,
                               std::shared_ptr<graphflow::core::GraphNode> feedback_sink = {});
  std::future<TrackId> subscribe(TrackDescriptor,
                                 std::shared_ptr<graphflow::core::GraphNode> consumer);
  // Factories run on the engine task queue after endpoint confirmation. They create
  // capture/codec/recording/playout nodes in the endpoint's own GraphEngine.
  // Internal failure cleanup runs before the operation's future becomes ready,
  // including cancellation and teardown. It must not throw or reenter this engine.
  using FailureCleanup = std::function<void(std::exception_ptr)>;
  std::future<TrackId> publish_graph(TrackDescriptor, PublishGraphFactory, FailureCleanup = {});
  std::future<TrackId> subscribe_graph(TrackDescriptor, SubscribeGraphFactory, FailureCleanup = {});
  std::future<void> unpublish(const TrackId&);
  std::future<void> unsubscribe(const TrackId&);
  WriteStatus write(const TrackId&, EncodedFrame);
  TrackSnapshot publication(const TrackId&) const;
  SenderFeedback feedback(const TrackId&) const;
  void complete_operations();  // Invoked by backend completion notifications.
  void check_health();

  struct GraphFailure {
    TrackId id;
    bool publication;
    std::string message;
  };

  void set_failure_handler(std::function<void(GraphFailure)> handler);
  void clear();

 private:
  void require_owner() const;
  void remove_graph(const TrackId&, bool publication);
  void cancel_pending(const TrackId&, bool publication);
  void failed_graph(const TrackId&, bool publication, std::string message);

  struct Pending {
    TrackId id;
    bool publication;
    std::function<bool()> complete;
    std::function<void(std::exception_ptr)> cancel;
  };

  RoomEngine* room_;
  const std::thread::id owner_ = std::this_thread::get_id();
  mutable std::mutex mutex_;
  std::map<TrackId, std::shared_ptr<PublishPipeline>> publications_;
  std::map<TrackId, std::shared_ptr<SubscribePipeline>> subscriptions_;
  std::vector<Pending> pending_;
  std::function<void(GraphFailure)> failure_handler_;
};

}  // namespace livekit::sdk::rtc::engine
