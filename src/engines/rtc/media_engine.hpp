#pragma once
#include "engines/rtc/media_module.hpp"
#include "engines/rtc/pipeline_engine.hpp"
#include <functional>
#include <future>
#include <memory>
#include <vector>

namespace livekit::sdk::rtc::engine {
// Borrows the facade-owned pipeline coordinator; never owns or closes a room.
class RtcMediaEngine final : public MediaModule {
 public:
  explicit RtcMediaEngine(RtcPipelineEngine&, AudioOutputOptions);
  MediaOperation publish_media(MediaPublishSpec) override;
  MediaOperation subscribe_media(MediaSubscribeSpec) override;
  std::shared_ptr<SharedMediaInput> create_shared_input(TrackDescriptor, MediaInput) override;
  void finish_pending() override;

 private:
  MediaOperation operation(std::future<TrackId>, std::shared_ptr<MediaTrack::State>);
  std::shared_ptr<AudioOutputSession> output_;
  RtcPipelineEngine* engine_;
  std::vector<std::function<bool()>> pending_;
};
}  // namespace livekit::sdk::rtc::engine
