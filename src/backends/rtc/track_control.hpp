#pragma once
#include "backends/rtc/control_context.hpp"
#include "livekit/async_result.hpp"
#include "livekit/rtc/track_endpoint.hpp"

namespace livekit::sdk::rtc::detail {
// Engine-owned access port. Endpoints keep only a weak handle; protocol state
// never escapes through it. Every function marshals through the owner's gate.
struct TrackControl {
  std::shared_ptr<SessionControl> control;
  std::function<AsyncResult<ErrorCode>(TrackId, bool, ControlOptions, std::shared_ptr<Endpoint>)>
      set_muted_async;
  std::function<AsyncResult<ErrorCode>(TrackId, ReceiveDemand, ControlOptions,
                                       std::shared_ptr<Endpoint>)>
      set_receive_demand_async;
  std::function<AsyncResult<ErrorCode>(std::shared_ptr<Endpoint>)> close_track_async;
  std::function<AsyncResult<ErrorCode>(std::shared_ptr<Endpoint>)> request_keyframe_async;
  std::function<void(std::shared_ptr<Endpoint>, std::exception_ptr)> report_failure;

  ErrorCode close_track(const std::shared_ptr<Endpoint>& endpoint) {
    return control_boundary([&] { return close_track_async(endpoint).get(); });
  }

  ErrorCode request_keyframe(const std::shared_ptr<Endpoint>& endpoint) {
    return control_boundary([&] { return request_keyframe_async(endpoint).get(); });
  }
};
}  // namespace livekit::sdk::rtc::detail
