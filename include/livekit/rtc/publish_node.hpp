#pragma once
#include "livekit/rtc/room_engine.hpp"
#include "livekit/rtc/events.hpp"
#include "core/graph_node.h"
#include <mutex>
#include <thread>

namespace livekit::sdk::rtc::nodes {

// One logical publication, potentially with multiple declared encodings. This
// sink never joins/leaves a room, creates a codec, or owns another media node.
class RtcPublishNode final : public graphflow::core::GraphNode {
 public:
  RtcPublishNode(std::string name, std::shared_ptr<PublishedTrack> track);
  ~RtcPublishNode() override;
  // Start the feedback worker after graph start; returns after initial feedback
  // has been delivered on that worker. Then activate upstream media producers.
  void activate();
  void quiesce();
  void throw_if_failed() const;

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
  mutable std::mutex failure_mutex_;
  std::exception_ptr failure_;
  std::jthread worker_;
};
}  // namespace livekit::sdk::rtc::nodes
