#pragma once
#include "livekit/result.hpp"
#include "livekit/rtc/track_endpoint.hpp"

namespace livekit::sdk::rtc {
// Completed control result. A non-null failure retains backend/context details.
struct OperationResult {
  ErrorCode code = ErrorCode::ok;
  std::shared_ptr<const FailureInfo> failure;

  explicit operator bool() const noexcept {
    return code == ErrorCode::ok;
  }
};

// Observers are borrowed through weak_ptr. Keep them alive while notifications
// are wanted. Notifications run serially on the room notification thread, outside
// control locks. They may submit asynchronous controls, but must not block, throw,
// or destroy the engine. Replacing an observer does not transfer pending results
// to the new observer; an invocation already running may finish after replacement.
class RoomObserver {
 public:
  virtual ~RoomObserver() = default;

  virtual void on_join_completed(const OperationResult&) {}

  virtual void on_leave_completed(const OperationResult&) {}

  virtual void on_close_completed(const OperationResult&) {}

  virtual void on_participant_update_completed(const OperationResult&) {}

  virtual void on_subscription_permissions_completed(const OperationResult&) {}

  virtual void on_room_event(const RoomEvent&) {}

  virtual void on_video_codec_policy_changed(const VideoCodecPolicySnapshot&) {}
};

class PublicationObserver {
 public:
  virtual ~PublicationObserver() = default;

  virtual void on_publish_completed(const TrackId&, Result<std::shared_ptr<PublishedTrack>>) {}

  virtual void on_unpublish_completed(const TrackId&, const OperationResult&) {}

  virtual void on_muted_completed(const TrackId&, bool, const OperationResult&) {}

  virtual void on_publication_event(const RoomEvent&) {}
};

class SubscriptionObserver {
 public:
  virtual ~SubscriptionObserver() = default;

  virtual void on_subscribe_completed(const TrackId&, Result<std::shared_ptr<SubscribedTrack>>) {}

  virtual void on_unsubscribe_completed(const TrackId&, const OperationResult&) {}

  virtual void on_receive_demand_completed(const TrackId&, ReceiveDemand, const OperationResult&) {}

  virtual void on_subscription_event(const RoomEvent&) {}
};
}  // namespace livekit::sdk::rtc
