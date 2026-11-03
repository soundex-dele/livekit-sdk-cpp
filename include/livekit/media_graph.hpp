#pragma once
#include "livekit/media_engine.hpp"
#include "livekit/rtc/input_node.hpp"
#include "engine/graph_builder.h"
#include <functional>
#include <utility>

namespace livekit::sdk::client {
// Opt-in GraphEngine API; link LiveKit::MediaEngineGraph. Room remains graph-independent.
// Nodes must be fresh, uniquely named, and exclusively owned by this pipeline.
// On adoption the engine owns init/start/stop/release; never drive nodes externally.
struct MediaGraph {
  std::vector<std::shared_ptr<graphflow::core::GraphNode>> nodes;
  std::vector<graphflow::engine::NodeConnection> connections;

  template <class Event>
  void connect(std::string from, std::string to) {
    connections.push_back(
        {std::move(from), std::move(to), graphflow::core::detail::prettyName<Event>(), {}});
  }

  // Optional synchronous hooks for custom producers. activate runs after all
  // nodes start; false fails publication/subscription. quiesce must fence custom
  // workers before downstream nodes stop, including partial initialization failure.
  // Hooks may report precise failures through GraphNode::reportError.
  std::function<bool()> activate;
  std::function<void()> quiesce;
};

struct MediaGraphContext {
  bool publishing = false;
  rtc::TrackDescriptor track;
  // Defaults contain room.port and the configured capture/codec/output nodes.
  // Keep the exact port node. Other nodes/connections may be replaced or extended.
  std::string port, input, codec, output;
  MediaGraph defaults;
};

struct MediaGraphOptions {
  // Called once synchronously by publish_media/subscribe_media, before init.
  // Return a fresh graph each time. No lifecycle/Room reentry or exceptions.
  // All custom node callbacks must also avoid engine lifecycle calls and blocking
  // work on producer threads. Use bounded workers for expensive processing.
  std::function<Result<MediaGraph>(MediaGraphContext)> build;
};
}  // namespace livekit::sdk::client
