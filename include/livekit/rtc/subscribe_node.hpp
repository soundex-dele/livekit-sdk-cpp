#pragma once
#include "livekit/compat/thread.hpp"
#include "livekit/rtc/track_endpoint.hpp"
#include "livekit/rtc/events.hpp"
#include "core/graph_node.h"
#include <mutex>
#include <thread>

namespace livekit::sdk::rtc::nodes {

// One subscription source with its own blocking receive worker. The endpoint
// wakes this worker; graph delivery never runs on the control/network thread.
// Lifecycle uses the graph owner. Consumers must not synchronously close the
// graph from a delivery callback. Quiesce the source before its downstream nodes.
class RtcSubscribeNode final : public graphflow::core::GraphNode {
 public:
  RtcSubscribeNode(std::string name, std::shared_ptr<SubscribedTrack> track);
  ~RtcSubscribeNode() override;
  Result<void> activate();  // After all graph nodes and downstream producers are ready.
  void quiesce();

  // Install before activate; invoked on the source worker, never during quiesce.
  void set_terminal_handler(std::function<void()> handler) {
    terminal_ = std::move(handler);
  }

  Result<void> status() const;

  std::shared_ptr<SubscribedTrack> track() const {
    return track_;
  }

  uint64_t delivered() const {
    return delivered_;
  }

 protected:
  bool doInit() override;
  bool doStart() override;
  void doStop() override;

 private:
  std::shared_ptr<SubscribedTrack> track_;
  std::atomic<bool> active_{false};
  std::atomic<uint64_t> delivered_{0};
  mutable std::mutex failure_mutex_;
  Failure failure_;
  livekit::compat::jthread worker_;
  std::function<void()> terminal_;
};
}  // namespace livekit::sdk::rtc::nodes
