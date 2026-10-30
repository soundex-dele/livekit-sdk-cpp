#pragma once
#include "livekit/result.hpp"
#include "livekit/rtc/track_endpoint.hpp"

namespace livekit::sdk::rtc {
// Observers are borrowed through weak_ptr. Keep them alive while notifications
// are wanted. Notifications run serially on the room notification thread, outside
// control locks. They may submit asynchronous controls, but must not block, throw,
// or destroy the engine. Observers receive only ongoing events; replacing them
// does not affect operation completions. Already running invocations may finish.
class RoomObserver {
 public:
  virtual ~RoomObserver() = default;

  virtual void on_room_event(const RoomEvent&) {}

  virtual void on_video_codec_policy_changed(const VideoCodecPolicySnapshot&) {}
};

class PublicationObserver {
 public:
  virtual ~PublicationObserver() = default;

  virtual void on_publication_event(const RoomEvent&) {}
};

class SubscriptionObserver {
 public:
  virtual ~SubscriptionObserver() = default;

  virtual void on_subscription_event(const RoomEvent&) {}
};
}  // namespace livekit::sdk::rtc
