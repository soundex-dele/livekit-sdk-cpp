#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/livekit.hpp"
#include "backends/rtc/control_context.hpp"
namespace livekit::sdk::rtc::engine {
std::unique_ptr<RtcEngine> create_livekit_engine(ErrorCode* error) {
  return detail::return_value(
      detail::control_boundary([&] {
        ErrorCode code;
        auto room = create_livekit_room(&code);
        if (!room) {
          return Result<std::unique_ptr<RtcEngine>>::failure(
              std::make_shared<FailureInfo>(FailureInfo{next_failure_id(), ErrorDomain::lifecycle,
                                                        code, "Room creation failed"}));
        }
        return Result<std::unique_ptr<RtcEngine>>::success(
            std::make_unique<RtcEngine>(std::move(room)));
      }),
      error);
}
}  // namespace livekit::sdk::rtc::engine
