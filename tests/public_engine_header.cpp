// This target deliberately has no GraphFlow include directories or libraries.
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/executor.hpp"
#include <type_traits>

static_assert(!std::is_copy_constructible_v<livekit::sdk::rtc::engine::RtcEngine>);
static_assert(!std::is_copy_constructible_v<livekit::sdk::rtc::engine::RtcExecutor>);
