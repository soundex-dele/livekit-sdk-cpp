#pragma once
#include "livekit/rtc/audio_nodes.hpp"
#include "livekit/rtc/video_nodes.hpp"
#include "media_graph/sync_clock.hpp"

namespace livekit::sdk::rtc::engine {
class MediaSyncNode final : public graphflow::core::GraphNode {
 public:
  explicit MediaSyncNode(std::shared_ptr<detail::MediaSyncClock> clock)
      : GraphNode("presentation.clock", nullptr), clock_(std::move(clock)) {}

 private:
  bool doInit() override {
    subscribe<nodes::VideoFrameEvent>([this](const auto& input) {
      auto output = input;
      clock_->map(output);
      publish(output);
    });
    subscribe<nodes::AudioFrameEvent>([this](const auto& input) {
      auto output = input;
      clock_->map(output);
      publish(output);
    });
    return true;
  }

  std::shared_ptr<detail::MediaSyncClock> clock_;
};
}  // namespace livekit::sdk::rtc::engine
