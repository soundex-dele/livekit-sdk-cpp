#pragma once
// Private protocol supervisors used only by contract/integration tests.
#include "rtc_blocking_bridge.hpp"
#include "rtc_room_fixture.hpp"

namespace livekit::sdk::app_support {
inline Result<std::unique_ptr<BlockingRoom>> create_blocking_room() {
  try {
    return Result<std::unique_ptr<BlockingRoom>>::success(
        std::make_unique<BlockingRoom>(rtc::detail::livekit_transport_factory()));
  } catch (...) {
    return Result<std::unique_ptr<BlockingRoom>>::failure(
        describe_failure(std::current_exception()));
  }
}

}  // namespace livekit::sdk::app_support
