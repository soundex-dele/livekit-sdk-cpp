#include "livekit/rtc/engine.hpp"
#include "backends/rtc/livekit/transport_factory.hpp"

namespace livekit::sdk::rtc::engine {
std::unique_ptr<RtcEngine> create_livekit_engine() {
  return std::make_unique<RtcEngine>(detail::livekit_transport_factory());
}
}  // namespace livekit::sdk::rtc::engine
