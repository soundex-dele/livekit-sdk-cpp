#pragma once
#include "livekit/media_engine.hpp"
#include "livekit/rtc/input_node.hpp"
#include "livekit/rtc/track_graph.hpp"
#include <functional>
#include <utility>

namespace livekit::sdk::client {
// Opt-in typed track graph API; link LiveKit::MediaEngineGraph. Room remains graph-independent.
// Nodes must be fresh, uniquely named, and exclusively owned by this pipeline.
// On adoption the engine owns init/start/stop/release; never drive nodes externally.
struct MediaGraph {
  std::vector<std::shared_ptr<rtc::TrackNode>> nodes;
  std::vector<rtc::TrackConnection> connections;

  template <class Event>
  void connect(std::string from, std::string to) {
    connections.push_back(rtc::TrackConnection::between<Event>(std::move(from), std::move(to)));
  }

  // Optional synchronous hooks for custom producers. activate runs after all
  // nodes start; false fails publication/subscription. quiesce must fence custom
  // workers before downstream nodes stop, including partial initialization failure.
  // Hooks may report precise failures through TrackNode::report_failure.
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
