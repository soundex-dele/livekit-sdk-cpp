#include "backends/rtc/legacy_control_boundary.hpp"
#include "livekit/rtc/engine.hpp"
#include "engines/rtc/media_graph_builder.hpp"
#include "backends/rtc/execution.hpp"

namespace livekit::sdk::rtc::engine {
ErrorCode RtcEngine::enable_media(AudioOutputOptions options) {
  return execution_->lifecycle.invoke([this, options = std::move(options)]() mutable {
    return detail::control_boundary([&] {
      media_resources_ = std::make_unique<RtcMediaResources>(std::move(options));
      media_ = std::make_unique<RtcMediaGraphBuilder>(
          static_cast<RtcMediaResources&>(*media_resources_));
    });
  });
}
}  // namespace livekit::sdk::rtc::engine
