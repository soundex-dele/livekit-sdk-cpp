#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/executor.hpp"

namespace livekit::sdk::rtc::engine {
std::unique_ptr<RtcEngine> create_livekit_engine(AudioOutputOptions options) {
  auto engine = create_livekit_engine();
  engine->enable_media(options).get();
  return engine;
}

std::unique_ptr<RtcExecutor> create_livekit_executor(AudioOutputOptions audio,
                                                     ExecutorOptions options) {
  return std::make_unique<RtcExecutor>([audio] { return create_livekit_engine(audio); }, options);
}
}  // namespace livekit::sdk::rtc::engine
