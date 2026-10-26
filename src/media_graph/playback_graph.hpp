#pragma once
#include "media_nodes/playback_output.hpp"
#include <atomic>

namespace livekit::sdk::detail {
// GraphBuilder connects typed pub/sub edges. Nodes own processing and buffering;
// the generic executor services them on the Player worker.
class PlaybackGraph {
 public:
  enum class Step { progress, backpressured, end };
  PlaybackGraph(std::atomic<bool>& cancel, const PlayerOptions& options);
  ~PlaybackGraph();
  PlayerMediaInfo open(const std::string& path);
  Step step(); // One bounded executor sweep; media travels through subscriptions.
  int64_t end_us() const;
  std::shared_ptr<PlaybackOutputs> outputs() const;
  // Internal diagnostics for lifecycle/topology validation; no backend API leaks.
  struct NodeInfo {
    std::string name;
    graphflow::core::LifecycleState state;
    uint64_t received = 0, published = 0;
  };
  std::vector<NodeInfo> nodes() const;
  std::vector<graphflow::core::GraphEdge> edges() const;
  void seek(int64_t position);
  void start();
  void pause();
 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
