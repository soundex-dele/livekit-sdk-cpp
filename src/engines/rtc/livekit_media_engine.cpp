#include "livekit/rtc/engine.hpp"

namespace livekit::sdk::rtc::engine {
Result<std::unique_ptr<RtcEngine>> create_livekit_engine(AudioOutputOptions options) {
  auto engine = create_livekit_engine();
  auto code = engine->enable_media(std::move(options));
  if (code != ErrorCode::ok) {
    return Result<std::unique_ptr<RtcEngine>>::failure(std::make_shared<FailureInfo>(
        FailureInfo{next_failure_id(), ErrorDomain::media, code, "Media configuration failed"}));
  }
  return Result<std::unique_ptr<RtcEngine>>::success(std::move(engine));
}
}  // namespace livekit::sdk::rtc::engine
