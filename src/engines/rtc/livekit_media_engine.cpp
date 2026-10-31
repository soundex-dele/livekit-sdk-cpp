#include "livekit/rtc/engine.hpp"

namespace livekit::sdk::rtc::engine {
std::unique_ptr<RtcEngine> create_livekit_engine(AudioOutputOptions options) {
  auto engine = create_livekit_engine();
  engine->enable_media(std::move(options));
  return engine;
}
}  // namespace livekit::sdk::rtc::engine
