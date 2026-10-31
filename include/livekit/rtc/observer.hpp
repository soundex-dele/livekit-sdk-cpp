#pragma once
#include "livekit/rtc/track_endpoint.hpp"

namespace livekit::sdk::rtc {
// One borrowed observer, kept alive until set_observer(nullptr) returns.
// Standalone RoomEngine delivers notifications on its notification worker.
// Callbacks may submit requests, but must not wait for other callbacks, leave,
// replace the observer or destroy the room. Media callbacks keep their own model.
class RoomObserver {
 public:
  virtual ~RoomObserver() = default;

  virtual void on_room_event(const RoomEvent&) {}
};
}  // namespace livekit::sdk::rtc
