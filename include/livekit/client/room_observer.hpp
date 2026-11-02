#pragma once

#include "livekit/client/room_event.hpp"

namespace livekit::sdk::client {
// Borrowed until unregister/replacement returns. Callbacks run serially on the
// notification thread. Only snapshot queries are allowed from this thread.
class RoomObserver {
 public:
  virtual ~RoomObserver() = default;

  virtual void on_room_event(const RoomEvent&) {}
};
}  // namespace livekit::sdk::client
