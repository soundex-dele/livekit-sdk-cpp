#include "livekit/rtc/engine.hpp"
#include "backends/rtc/livekit/transport_factory.hpp"
#include "backends/rtc/control_context.hpp"

namespace livekit::sdk::rtc::engine {
Result<std::unique_ptr<RtcEngine>> create_livekit_engine() {
  return detail::control_boundary([] {
    return Result<std::unique_ptr<RtcEngine>>::success(
        std::make_unique<RtcEngine>(detail::livekit_transport_factory()));
  });
}
}  // namespace livekit::sdk::rtc::engine
