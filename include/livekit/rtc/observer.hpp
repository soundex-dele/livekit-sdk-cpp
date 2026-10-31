#pragma once
#include "livekit/rtc/track_endpoint.hpp"

namespace livekit::sdk::rtc {
// One borrowed observer. The owner keeps it alive until it is detached.
// Notifications run inline on the notifying backend thread; on_left runs on
// the caller of leave(). Do not wait, leave or destroy the room in a backend
// notification. This minimal control layer does not synchronize callers.
class RoomObserver {
 public:
  virtual ~RoomObserver() = default;

  virtual void on_joined(const RoomSnapshot&) {}

  virtual void on_left() {}

  virtual void on_published(std::shared_ptr<PublishedTrack>) {}

  virtual void on_subscribed(std::shared_ptr<SubscribedTrack>) {}

  virtual void on_unpublished(const TrackId&) {}

  virtual void on_unsubscribed(const TrackId&) {}

  virtual void on_room_event(const RoomEvent&) {}
};
}  // namespace livekit::sdk::rtc
