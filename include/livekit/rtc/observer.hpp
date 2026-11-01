#pragma once
#include "livekit/rtc/track_endpoint.hpp"

namespace livekit::sdk::rtc {
// Borrowed until set_observer(nullptr) or replacement returns.
// All room observers run on the room notification worker.
// Callbacks may submit requests, but must not wait for other callbacks, leave,
// replace the observer or destroy the room. Media callbacks keep their own model.
class RoomObserver {
 public:
  virtual ~RoomObserver() = default;

  virtual void on_room_event(const RoomEvent&) {}
};
}  // namespace livekit::sdk::rtc
