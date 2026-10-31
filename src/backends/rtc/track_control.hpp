#pragma once
#include "backends/rtc/control_context.hpp"
#include "livekit/rtc/track_endpoint.hpp"
#include <functional>

namespace livekit::sdk::rtc::detail {
// Endpoints borrow these commands; releasing the room expires the port.
struct TrackControl {
  std::function<void(std::shared_ptr<Endpoint>)> send;
  std::function<void(std::shared_ptr<Endpoint>)> request_keyframe;
  std::function<void(const TrackId&, bool)> set_muted;
  std::function<void(const TrackId&, ReceiveDemand)> set_receive_demand;
  std::function<void(std::shared_ptr<Endpoint>)> close_track;
};
}  // namespace livekit::sdk::rtc::detail
