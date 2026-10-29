#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/livekit.hpp"

namespace livekit::sdk::rtc::engine {
std::unique_ptr<RtcEngine> create_livekit_engine() {
  return std::make_unique<RtcEngine>(create_livekit_room());
}
} // namespace livekit::sdk::rtc::engine
