#pragma once
#include "engines/rtc/media_graph_contract.hpp"
#include "livekit/rtc/audio_output.hpp"
#include "media_graph/sync_clock.hpp"
#include <map>

namespace livekit::sdk::rtc::engine {
// Session-scoped devices and clocks outlive all graphs and the builder.
struct RtcMediaResources final : MediaResources {
  explicit RtcMediaResources(AudioOutputOptions);
  std::shared_ptr<AudioRenderReference> echo_reference;
  std::shared_ptr<AudioOutputSession> output;
  std::map<std::pair<std::string, std::string>, std::weak_ptr<detail::MediaSyncClock>> clocks;
};
}  // namespace livekit::sdk::rtc::engine
