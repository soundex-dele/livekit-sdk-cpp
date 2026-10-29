#pragma once
#include "livekit/rtc/publish_node.hpp"
#include "livekit/rtc/subscribe_node.hpp"
#include "engine/graph_engine.h"
#include <functional>
#include <mutex>

namespace livekit::sdk::rtc::engine {
struct GraphObservation;
// Invoked on the reporting worker. Enqueue owner-thread lifecycle work; never
// stop or join the graph here. A null exception denotes normal endpoint closure.
using GraphStopped = std::function<void(std::exception_ptr)>;

// Additional nodes in the same per-track GraphEngine as its RTC endpoint.
// Nodes must be fresh. Connections may refer to the supplied RTC node. Start
// production in activate, after every node has started; quiesce must not throw
// and must join all producers before returning. Nodes actively report failures.
// Codec/device work belongs in bounded workers owned by these nodes.
struct TrackGraphSpec {
  std::vector<std::shared_ptr<graphflow::core::GraphNode>> nodes;
  std::vector<graphflow::engine::NodeConnection> connections;
  std::function<void()> activate;
  std::function<void()> quiesce;
  // Owner-thread notification before failed graph teardown. Must not throw or
  // reenter graph lifecycle. Retain the exception for snapshots/application queries.
  std::function<void(std::exception_ptr)> on_failure;

  template <class Event>
  static graphflow::engine::NodeConnection connect(
      const std::shared_ptr<graphflow::core::GraphNode>& from,
      const std::shared_ptr<graphflow::core::GraphNode>& to) {
    return {from->getName(), to->getName(), graphflow::core::detail::prettyName<Event>(), {}};
  }
};

using PublishGraphFactory = std::function<TrackGraphSpec(std::shared_ptr<nodes::RtcPublishNode>)>;
using SubscribeGraphFactory =
    std::function<TrackGraphSpec(std::shared_ptr<nodes::RtcSubscribeNode>)>;

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
  AsyncResult<void> close();

  const nodes::RtcPublishNode& node() const {
    return *sink_;
  }

 private:
  class Input;
  std::mutex mutex_;
  std::shared_ptr<Input> input_;
  std::shared_ptr<nodes::RtcPublishNode> sink_;
  TrackGraphSpec extension_;
  std::shared_ptr<GraphObservation> observation_;
  graphflow::engine::GraphEngine graph_{1};
  bool running_ = false, closed_ = false;
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
  AsyncResult<void> close();

  const nodes::RtcSubscribeNode& node() const {
    return *source_;
  }

 private:
  std::mutex mutex_;
  std::shared_ptr<nodes::RtcSubscribeNode> source_;
  TrackGraphSpec extension_;
  std::shared_ptr<GraphObservation> observation_;
  graphflow::engine::GraphEngine graph_{1};
  bool running_ = false, closed_ = false;
};
}  // namespace livekit::sdk::rtc::engine
