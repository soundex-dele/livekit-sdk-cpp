#pragma once
#include "livekit/rtc/room_backend.hpp"
#include "livekit/rtc/events.hpp"
#include "core/graph_node.h"

namespace livekit::sdk::rtc::nodes {

// One subscription source. The engine fairly polls all sources on its media
// scheduling sequence. No per-track thread, decoding or direct network callback.
class RtcSubscribeNode final : public graphflow::core::GraphNode {
 public:
  RtcSubscribeNode(std::string name, std::shared_ptr<SubscribedTrack> track);
  size_t poll(size_t max_frames = 4);
  std::shared_ptr<SubscribedTrack> track() const { return track_; }
  uint64_t delivered() const { return delivered_; }
 protected:
  bool doInit() override;
  bool doStart() override;
  void doStop() override;
 private:
  std::shared_ptr<SubscribedTrack> track_;
  std::atomic<bool> active_{false};
  uint64_t delivered_ = 0;
};
} // namespace livekit::sdk::rtc::nodes
