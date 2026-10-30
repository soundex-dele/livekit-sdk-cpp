#pragma once
#include "livekit/rtc/completion.hpp"
#include "livekit/async_result.hpp"
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
// Asynchronous controls return only admission status. Each accepted operation
// retains its completion until delivery; rejected operations produce none.
// Observers are optional and receive only ongoing events. Duplicate
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

  [[nodiscard]] ErrorCode join(RoomOptions options, Completion<void> completion);
  [[nodiscard]] ErrorCode publish(TrackDescriptor descriptor,
                                  Completion<std::shared_ptr<PublishedTrack>> completion);
  [[nodiscard]] ErrorCode subscribe(TrackDescriptor remote_track,
                                    Completion<std::shared_ptr<SubscribedTrack>> completion);
  // Completion follows local transport detachment.
  [[nodiscard]] ErrorCode unpublish(const TrackId& id, Completion<void> completion);
  [[nodiscard]] ErrorCode unsubscribe(const TrackId& id, Completion<void> completion);
  [[nodiscard]] ErrorCode set_muted(const TrackId& id, bool muted, Completion<void> completion,
                                    ControlOptions = {});
  [[nodiscard]] ErrorCode set_receive_demand(const TrackId& id, ReceiveDemand,
                                             Completion<void> completion, ControlOptions = {});
  // Replaces the publisher's allowlist. Success means signaling submission,
  // not server acknowledgement. Native policy intent survives rejoin even if
  // an interrupted submission has an unknown outcome.
  [[nodiscard]] ErrorCode set_subscription_permissions(SubscriptionPermissions,
                                                       Completion<void> completion,
                                                       ControlOptions = {});
  // Requires signaling protocol 15+. Resolves only on the server's ACK, not
  // after a local send. A dispatched operation interrupted/cancelled/timed out
  // can have an unknown outcome and is never automatically replayed.
  [[nodiscard]] ErrorCode update_local_participant(ParticipantUpdate, Completion<void> completion,
                                                   ControlOptions = {});
  [[nodiscard]] ErrorCode leave(Completion<void> completion = {});
  [[nodiscard]] ErrorCode close(Completion<void> completion = {});
  void set_room_observer(std::weak_ptr<RoomObserver>);
  void set_publication_observer(std::weak_ptr<PublicationObserver>);
  void set_subscription_observer(std::weak_ptr<SubscriptionObserver>);
  RoomSnapshot snapshot() const;
  Result<std::shared_ptr<RoomServices>> services() const;

 private:
  friend class engine::RtcEngine;
  RoomEngine(TransportFactory, std::shared_ptr<detail::SessionControl>);
  std::shared_ptr<detail::SessionControl> control() const;
  // Initiation holds only a short controller lock; settlement runs on the room pump.
  AsyncResult<ErrorCode> join_async(RoomOptions);
  AsyncResult<Result<std::shared_ptr<PublishedTrack>>> publish_async(TrackDescriptor);
  AsyncResult<Result<std::shared_ptr<SubscribedTrack>>> subscribe_async(TrackDescriptor);
  AsyncResult<ErrorCode> unpublish_async(const TrackId&);
  AsyncResult<ErrorCode> unsubscribe_async(const TrackId&);
  AsyncResult<ErrorCode> set_muted_async(const TrackId&, bool, ControlOptions);
  AsyncResult<ErrorCode> set_receive_demand_async(const TrackId&, ReceiveDemand, ControlOptions);
  AsyncResult<ErrorCode> update_local_participant_async(ParticipantUpdate, ControlOptions);
  AsyncResult<ErrorCode> set_subscription_permissions_async(SubscriptionPermissions,
                                                            ControlOptions);
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
// invoke their completion; retained service handles from a departed room stay closed.
class RoomView {
 public:
  RoomSnapshot snapshot() const {
    return room_->snapshot();
  }

  Result<std::shared_ptr<RoomServices>> services() const {
    return room_->services();
  }

  [[nodiscard]] ErrorCode update_local_participant(ParticipantUpdate update,
                                                   Completion<void> completion,
                                                   ControlOptions options = {}) const {
    return room_->update_local_participant(std::move(update), std::move(completion), options);
  }

  [[nodiscard]] ErrorCode set_subscription_permissions(SubscriptionPermissions policy,
                                                       Completion<void> completion,
                                                       ControlOptions options = {}) const {
    return room_->set_subscription_permissions(std::move(policy), std::move(completion), options);
  }

 private:
  friend class engine::RtcEngine;

  explicit RoomView(RoomEngine* room) : room_(room) {}

  RoomEngine* room_;
};
}  // namespace livekit::sdk::rtc
