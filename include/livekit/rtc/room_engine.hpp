#pragma once
#include "livekit/result.hpp"
#include "livekit/rtc/observer.hpp"
#include "livekit/rtc/track_endpoint.hpp"

#include "livekit/rtc/types.hpp"
#include "livekit/rtc/room_services.hpp"
#include <functional>
#include <stop_token>
#include <memory>
#include <optional>
#include <utility>

namespace livekit::sdk::rtc {
// Asynchronous controls return only admission status. Register the relevant
// observer before submitting. Each accepted operation produces one completion
// while its observer remains alive; rejected operations produce none. Duplicate
// operations on the same object return busy until completion delivery starts.
// leave/close cancel pending work. Destruction drains workers/notifications and
// must happen outside callbacks, after application callers have stopped.
class RoomEngine {
 public:
  // Factory must return a fresh adapter. It is retained and called again after leave.
  using TransportFactory = std::function<std::unique_ptr<detail::RoomTransport>()>;
  explicit RoomEngine(TransportFactory);
  ~RoomEngine();
  RoomEngine(const RoomEngine&) = delete;
  RoomEngine& operator=(const RoomEngine&) = delete;

  ErrorCode join(RoomOptions options);
  ErrorCode publish(TrackDescriptor descriptor);
  ErrorCode subscribe(TrackDescriptor remote_track);
  // Completion follows local transport detachment.
  ErrorCode unpublish(const TrackId& id);
  ErrorCode unsubscribe(const TrackId& id);
  ErrorCode set_muted(const TrackId& id, bool muted, ControlOptions = {});
  ErrorCode set_receive_demand(const TrackId& id, ReceiveDemand, ControlOptions = {});
  // Replaces the publisher's allowlist. Success means signaling submission,
  // not server acknowledgement. Native policy intent survives rejoin even if
  // an interrupted submission has an unknown outcome.
  ErrorCode set_subscription_permissions(SubscriptionPermissions, ControlOptions = {});
  // Requires signaling protocol 15+. Resolves only on the server's ACK, not
  // after a local send. A dispatched operation interrupted/cancelled/timed out
  // can have an unknown outcome and is never automatically replayed.
  ErrorCode update_local_participant(ParticipantUpdate, ControlOptions = {});
  ErrorCode leave();
  ErrorCode close();
  void set_room_observer(std::weak_ptr<RoomObserver>);
  void set_publication_observer(std::weak_ptr<PublicationObserver>);
  void set_subscription_observer(std::weak_ptr<SubscriptionObserver>);
  RoomSnapshot snapshot() const;
  Result<std::shared_ptr<RoomServices>> services() const;

 private:
  friend class engine::RtcEngine;
  RoomEngine(TransportFactory, std::shared_ptr<detail::SessionControl>);
  std::shared_ptr<detail::SessionControl> control() const;
  ErrorCode join_locked(RoomOptions options);
  Result<std::shared_ptr<PublishedTrack>> publish_locked(TrackDescriptor descriptor);
  Result<std::shared_ptr<SubscribedTrack>> subscribe_locked(TrackDescriptor remote_track);
  // Completion follows local transport detachment.
  ErrorCode unpublish_locked(const TrackId& id);
  ErrorCode unsubscribe_locked(const TrackId& id);
  ErrorCode set_muted_locked(const TrackId& id, bool muted, ControlOptions = {});
  ErrorCode set_receive_demand_locked(const TrackId& id, ReceiveDemand, ControlOptions = {});
  // Replaces the publisher's allowlist. Success means signaling submission,
  // not server acknowledgement. Native policy intent survives rejoin even if
  // an interrupted submission has an unknown outcome.
  ErrorCode set_subscription_permissions_locked(SubscriptionPermissions, ControlOptions = {});
  // Requires signaling protocol 15+. Resolves only on the server's ACK, not
  // after a local send. A dispatched operation interrupted/cancelled/timed out
  // can have an unknown outcome and is never automatically replayed.
  ErrorCode update_local_participant_locked(ParticipantUpdate, ControlOptions = {});
  ErrorCode leave_locked();
  void set_event_handler_locked(std::function<void(const RoomEvent&)>);
  void set_internal_room_observer(std::weak_ptr<RoomObserver>);
  void post_notification(std::function<void()>);
  void notification_failed();
  std::shared_ptr<detail::RoomEngineState> state_;
};

// Restricted borrowed facade. Lifecycle/track mutation goes through RtcEngine.
// Snapshot/services are safe across threads. Controls submit asynchronously and
// notify RoomObserver; retained service handles from a departed room stay closed.
class RoomView {
 public:
  RoomSnapshot snapshot() const {
    return room_->snapshot();
  }

  Result<std::shared_ptr<RoomServices>> services() const {
    return room_->services();
  }

  ErrorCode update_local_participant(ParticipantUpdate update, ControlOptions options = {}) const {
    return room_->update_local_participant(std::move(update), options);
  }

  ErrorCode set_subscription_permissions(SubscriptionPermissions policy,
                                         ControlOptions options = {}) const {
    return room_->set_subscription_permissions(std::move(policy), options);
  }

 private:
  friend class engine::RtcEngine;

  explicit RoomView(RoomEngine* room) : room_(room) {}

  RoomEngine* room_;
};
}  // namespace livekit::sdk::rtc
