#pragma once
#include "livekit/rtc/audio_nodes.hpp"
#include "livekit/rtc/video_nodes.hpp"
#include "media_graph/sync_clock.hpp"

namespace livekit::sdk::rtc::engine {
class MediaSyncNode final : public livekit::sdk::rtc::TrackNode {
 public:
  explicit MediaSyncNode(std::shared_ptr<detail::MediaSyncClock> clock)
      : TrackNode("presentation.clock"), clock_(std::move(clock)) {}

 private:
  void bind(graphflow::Binder& ports) override {
    output<nodes::AudioFrameEvent>(ports);
    output<nodes::VideoFrameEvent>(ports);

    ports.input(livekit::sdk::rtc::TrackPort<nodes::VideoFrameEvent>::in,
                [this](const auto& input) {
                  auto output = input;
                  clock_->map(output);
                  send(output);
                });
    ports.input(livekit::sdk::rtc::TrackPort<nodes::AudioFrameEvent>::in,
                [this](const auto& input) {
                  auto output = input;
                  clock_->map(output);
                  send(output);
                });
  }

  std::shared_ptr<detail::MediaSyncClock> clock_;
};
}  // namespace livekit::sdk::rtc::engine
