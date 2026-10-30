#include "engines/rtc/rtc_engine_impl.hpp"
#include "engines/rtc/media_graph_builder.hpp"

namespace livekit::sdk::rtc::engine {
Result<void> RtcEngine::enable_media(AudioOutputOptions options) {
  return impl_->execute([state = impl_.get(), options = std::move(options)]() mutable {
    state->require_open();
    if (state->media || state->room->snapshot().state != ConnectionState::disconnected) {
      throw std::logic_error("Install media once before joining a room");
    }
    auto resources = std::make_unique<RtcMediaResources>(std::move(options));
    auto builder = std::make_unique<RtcMediaGraphBuilder>(*resources);
    state->media_resources = std::move(resources);
    state->media = std::move(builder);
    state->has_media = true;
  });
}
}  // namespace livekit::sdk::rtc::engine
