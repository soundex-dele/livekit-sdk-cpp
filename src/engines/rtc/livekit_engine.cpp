#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/executor.hpp"
#include "livekit/rtc/livekit.hpp"

namespace livekit::sdk::rtc::engine {
std::unique_ptr<RtcEngine> create_livekit_engine() {
  return std::make_unique<RtcEngine>(create_livekit_room());
}

std::unique_ptr<RtcExecutor> create_livekit_executor(ExecutorOptions options) {
  return std::make_unique<RtcExecutor>([] { return create_livekit_engine(); }, options);
}
}  // namespace livekit::sdk::rtc::engine
