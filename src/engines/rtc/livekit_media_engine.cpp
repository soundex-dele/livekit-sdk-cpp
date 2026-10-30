#include "livekit/rtc/engine.hpp"
namespace livekit::sdk::rtc::engine {
std::unique_ptr<RtcEngine> create_livekit_engine(AudioOutputOptions options, ErrorCode* error) {
  auto engine = create_livekit_engine(error);
  if (!engine) {
    return {};
  }
  const auto enabled = engine->enable_media(std::move(options));
  if (error) {
    *error = enabled;
  }
  return enabled == ErrorCode::ok ? std::move(engine) : nullptr;
}
}  // namespace livekit::sdk::rtc::engine
