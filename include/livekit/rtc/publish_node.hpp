#pragma once
#include "livekit/rtc/room_engine.hpp"
#include "livekit/rtc/events.hpp"
#include "core/graph_node.h"

namespace livekit::sdk::rtc::nodes {

// One logical publication, potentially with multiple declared encodings. This
// sink never joins/leaves a room, creates a codec, or owns another media node.
class RtcPublishNode final : public graphflow::core::GraphNode {
 public:
  RtcPublishNode(std::string name, std::shared_ptr<PublishedTrack> track);
  void poll_feedback();  // Engine control sequence; never a native RTC callback.

  WriteStatus last_write() const {
    return last_write_.load();
  }

  std::shared_ptr<PublishedTrack> track() const {
    return track_;
  }

  uint64_t accepted() const {
    return accepted_.load();
  }

  uint64_t rejected() const {
    return rejected_.load();
  }

 protected:
  bool doInit() override;
  bool doStart() override;
  void doStop() override;

 private:
  std::shared_ptr<PublishedTrack> track_;
  std::atomic<bool> active_{false};
  std::atomic<WriteStatus> last_write_{WriteStatus::not_ready};
  std::atomic<uint64_t> accepted_{0}, rejected_{0};
  uint64_t feedback_revision_ = UINT64_MAX;
  Epoch feedback_epoch_ = 0;
};
}  // namespace livekit::sdk::rtc::nodes
