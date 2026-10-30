#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/livekit.hpp"
#include "backends/rtc/control_context.hpp"
namespace livekit::sdk::rtc::engine {
Result<std::unique_ptr<RtcEngine>> create_livekit_engine() {
  return detail::control_boundary([] {
    auto room = create_livekit_room();
    if (!room) {
      return Result<std::unique_ptr<RtcEngine>>::failure(room.failure_record());
    }
    return Result<std::unique_ptr<RtcEngine>>::success(
        std::make_unique<RtcEngine>(*std::move(room)));
  });
}
}  // namespace livekit::sdk::rtc::engine
