#include "livekit/rtc/engine.hpp"
namespace livekit::sdk::rtc::engine {
Result<std::unique_ptr<RtcEngine>> create_livekit_engine(AudioOutputOptions options) {
  auto engine = create_livekit_engine();
  if (!engine) {
    return engine;
  }
  auto enabled = (*engine)->enable_media(std::move(options));
  if (!enabled) {
    return Result<std::unique_ptr<RtcEngine>>::failure(enabled.failure_record());
  }
  return engine;
}
}  // namespace livekit::sdk::rtc::engine
