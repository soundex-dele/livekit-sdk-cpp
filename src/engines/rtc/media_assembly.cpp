#include "livekit/rtc/engine.hpp"
#include "engines/rtc/media_graph_builder.hpp"

namespace livekit::sdk::rtc::engine {
void RtcEngine::enable_media(AudioOutputOptions options) {
  media_resources_ = std::make_unique<RtcMediaResources>(std::move(options));
  media_ =
      std::make_unique<RtcMediaGraphBuilder>(static_cast<RtcMediaResources&>(*media_resources_));
}
}  // namespace livekit::sdk::rtc::engine
