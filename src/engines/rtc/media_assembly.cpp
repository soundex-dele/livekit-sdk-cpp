#include "engines/rtc/rtc_engine_impl.hpp"
#include "engines/rtc/media_graph_builder.hpp"

namespace livekit::sdk::rtc::engine {
ErrorCode RtcEngine::enable_media(AudioOutputOptions options) {
  return impl_->execute([state = impl_.get(), options = std::move(options)]() mutable {
    if (state->media || state->room->snapshot().state != ConnectionState::disconnected) {
      return ErrorCode::invalid_state;
    }
    auto resources = std::make_unique<RtcMediaResources>(std::move(options));
    auto builder = std::make_unique<RtcMediaGraphBuilder>(*resources);
    state->media_resources = std::move(resources);
    state->media = std::move(builder);
    state->has_media = true;
    return ErrorCode::ok;
  });
}
}  // namespace livekit::sdk::rtc::engine
