#pragma once
#include "livekit/rtc/publish_node.hpp"
#include "livekit/rtc/subscribe_node.hpp"
#include "engine/graph_builder.h"
#include <functional>
#include <vector>

namespace livekit::sdk::rtc::engine {
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

}  // namespace livekit::sdk::rtc::engine
