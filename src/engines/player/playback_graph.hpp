#pragma once
#include "media_nodes/render/playback_output.hpp"
#include <atomic>

namespace livekit::sdk::detail {
// GraphFlow Inline connects typed ports. Nodes own processing and buffering;
// the generic executor services them on the Player worker.
class PlaybackGraph {
 public:
  enum class Step { progress, backpressured, end };
  PlaybackGraph(std::atomic<bool>& cancel, const PlayerOptions& options);
  ~PlaybackGraph();
  Result<PlayerMediaInfo> open(const std::string& path);
  Result<Step> step(const MediaExecutor::PumpProbe& probe =
                        {});  // One bounded executor sweep; media travels through subscriptions.
  int64_t end_us() const;
  std::shared_ptr<PlaybackOutputs> outputs() const;

  // Internal diagnostics for lifecycle/topology validation; no backend API leaks.
  struct NodeInfo {
    std::string name;
    MediaState state;
    uint64_t received = 0, published = 0;
  };

  std::vector<NodeInfo> nodes() const;
  std::vector<MediaEdge> edges() const;
  Result<void> seek(int64_t position);
  Result<void> start();
  Result<void> pause();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk::detail
