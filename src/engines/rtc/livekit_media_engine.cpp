#include "livekit/rtc/engine.hpp"
#include "backends/rtc/control_context.hpp"

namespace livekit::sdk::rtc::engine {
Result<std::unique_ptr<RtcEngine>> create_livekit_engine(AudioOutputOptions options) {
  return detail::control_boundary([&] {
    auto engine = create_livekit_engine();
    if (!engine) {
      return engine;
    }
    const auto enabled = (*engine)->enable_media(std::move(options));
    if (enabled != ErrorCode::ok) {
      return detail::failure_result<std::unique_ptr<RtcEngine>>(enabled,
                                                                "Failed to enable RTC media");
    }
    return engine;
  });
}
}  // namespace livekit::sdk::rtc::engine
