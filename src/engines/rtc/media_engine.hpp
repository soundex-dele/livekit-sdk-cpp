#pragma once
#include "engines/rtc/media_module.hpp"
#include "livekit/rtc/audio_output.hpp"
#include "engines/rtc/pipeline_engine.hpp"
#include <functional>
#include <future>
#include <memory>
#include <vector>
#include "media_graph/sync_clock.hpp"

namespace livekit::sdk::rtc::engine {
// Borrows the facade-owned pipeline coordinator; never owns or closes a room.
class RtcMediaEngine final : public MediaModule {
 public:
  explicit RtcMediaEngine(RtcPipelineEngine&, AudioOutputOptions);
  Result<MediaPublication> publish_media(MediaPublishSpec) override;
  Result<MediaSubscription> subscribe_media(MediaSubscribeSpec) override;
  std::shared_ptr<SharedMediaInput> create_shared_input(TrackDescriptor, MediaInput) override;

 private:
  static RtcPipelineEngine::FailureCleanup failure_cleanup(std::shared_ptr<MediaTrack::State>);
  std::shared_ptr<AudioRenderReference> echo_reference_;
  std::shared_ptr<AudioOutputSession> output_;
  RtcPipelineEngine* engine_;
  std::map<std::pair<std::string, std::string>, std::weak_ptr<detail::MediaSyncClock>> clocks_;
};
}  // namespace livekit::sdk::rtc::engine
