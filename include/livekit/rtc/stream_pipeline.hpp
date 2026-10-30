#pragma once
#include "livekit/rtc/track_graph.hpp"
#include "engine/graph_engine.h"
#include <functional>
#include <mutex>

namespace livekit::sdk::rtc::engine {
struct GraphObservation;
// Invoked on the reporting worker. Enqueue owner-thread lifecycle work; never
// stop or join the graph here. A null exception denotes normal endpoint closure.
using GraphStopped = std::function<void(std::exception_ptr)>;

// Engine-owned topology for one logical track. Lifecycle operations use one
// owner sequence; write() is safe from producers during lifecycle transitions.
// Nodes hold endpoint leases, never a RoomEngine.
// To add/remove a stream, create/close this graph without rebuilding other graphs.
class PublishPipeline {
 public:
  explicit PublishPipeline(std::shared_ptr<PublishedTrack>,
                           std::shared_ptr<graphflow::core::GraphNode> feedback_sink = {});
  PublishPipeline(std::shared_ptr<PublishedTrack>, PublishGraphFactory, GraphStopped = {});
  ~PublishPipeline();
  void start();
  WriteStatus write(EncodedFrame);
  std::exception_ptr failure() const;
  void notify_failure(std::exception_ptr);  // Owner sequence only, before close.
  Result<void> close();

  const nodes::RtcPublishNode& node() const {
    return *sink_;
  }

 private:
  friend class TrackGraphRegistry;
  friend class RtcEngine;
  void start_locked();
  Result<void> close_locked();
  PublishPipeline(std::shared_ptr<nodes::RtcPublishNode>, TrackGraphSpec, GraphStopped);
  void prepare();
  class Input;
  std::mutex mutex_;
  std::shared_ptr<Input> input_;
  std::shared_ptr<nodes::RtcPublishNode> sink_;
  TrackGraphSpec extension_;
  std::shared_ptr<GraphObservation> observation_;
  graphflow::engine::GraphEngine graph_{1};
  bool running_ = false, closed_ = false;
  bool owns_endpoint_ = true, prepared_ = false;
  bool failure_notified_ = false;
};

class SubscribePipeline {
 public:
  // The consumer receives EncodedFrameEvent. Decoding/recording belong in that
  // consumer's engine graph, never inside the RTC backend or source node.
  SubscribePipeline(std::shared_ptr<SubscribedTrack>,
                    std::shared_ptr<graphflow::core::GraphNode> consumer);
  SubscribePipeline(std::shared_ptr<SubscribedTrack>, SubscribeGraphFactory, GraphStopped = {});
  ~SubscribePipeline();
  void start();
  std::exception_ptr failure() const;
  void notify_failure(std::exception_ptr);  // Owner sequence only, before close.
  Result<void> close();

  const nodes::RtcSubscribeNode& node() const {
    return *source_;
  }

 private:
  friend class TrackGraphRegistry;
  friend class RtcEngine;
  void start_locked();
  Result<void> close_locked();
  std::mutex mutex_;
  SubscribePipeline(std::shared_ptr<nodes::RtcSubscribeNode>, TrackGraphSpec, GraphStopped);
  void prepare();
  std::shared_ptr<nodes::RtcSubscribeNode> source_;
  TrackGraphSpec extension_;
  std::shared_ptr<GraphObservation> observation_;
  graphflow::engine::GraphEngine graph_{1};
  bool running_ = false, closed_ = false;
  bool owns_endpoint_ = true, prepared_ = false;
  bool failure_notified_ = false;
};
}  // namespace livekit::sdk::rtc::engine
