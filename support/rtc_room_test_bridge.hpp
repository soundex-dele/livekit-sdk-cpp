#pragma once
// Private protocol supervisors used only by contract/integration tests.
#include "rtc_blocking_bridge.hpp"
#include "rtc_room_fixture.hpp"
#include "backends/rtc/livekit/transport_factory.hpp"

namespace livekit::sdk::app_support {
inline Result<std::unique_ptr<BlockingRoom>> create_blocking_room() {
  return Result<std::unique_ptr<BlockingRoom>>::success(
      std::make_unique<BlockingRoom>(rtc::detail::livekit_transport_factory()));
}

}  // namespace livekit::sdk::app_support
