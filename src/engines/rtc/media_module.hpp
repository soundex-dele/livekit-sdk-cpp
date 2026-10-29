#pragma once
#include "livekit/rtc/media_config.hpp"

namespace livekit::sdk::rtc::engine {
// Optional module boundary keeps the facade usable without codec/device libraries.
class MediaModule {
 public:
  virtual ~MediaModule() = default;
  virtual MediaOperation publish_media(MediaPublishSpec) = 0;
  virtual MediaOperation subscribe_media(MediaSubscribeSpec) = 0;
  virtual std::shared_ptr<SharedMediaInput> create_shared_input(TrackDescriptor, MediaInput) = 0;
  virtual void finish_pending() = 0;
};
}  // namespace livekit::sdk::rtc::engine
