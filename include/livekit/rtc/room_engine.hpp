#pragma once
#include "livekit/result.hpp"
#include "livekit/rtc/track_endpoint.hpp"

#include "livekit/rtc/types.hpp"
#include "livekit/rtc/room_services.hpp"
#include <functional>
#include <stop_token>
#include <memory>
#include <optional>
#include <utility>

namespace livekit::sdk::rtc {
// Controls execute on the calling thread, including protocol ACK waits. Backend
// events and media are serviced in the background when no control is running.
// Operations return ErrorCode::ok; creation/access returns nullptr on failure
// and always sets the optional error output. Backend exceptions do not escape.
// Invalid/null transport factories are reported by join().
// Transport-only room session. The private adapter is injected so the same state
// machine can be tested without network, devices or codecs. The native factory
// provides this adapter; callers do not implement GraphEngine or WebRTC objects.
class RoomEngine {
 public:
  // Factory must return a fresh adapter. It is retained and called again after leave.
  using TransportFactory = std::function<std::unique_ptr<detail::RoomTransport>()>;
  explicit RoomEngine(TransportFactory);
  ~RoomEngine();
  RoomEngine(const RoomEngine&) = delete;
  RoomEngine& operator=(const RoomEngine&) = delete;

  ErrorCode join(RoomOptions options);
  std::shared_ptr<PublishedTrack> publish(TrackDescriptor descriptor, ErrorCode* error = nullptr);
  std::shared_ptr<SubscribedTrack> subscribe(TrackDescriptor remote_track,
                                             ErrorCode* error = nullptr);
  // Waits for the preceding operation, including publication/subscription ACK.
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
  RoomSnapshot snapshot() const;
  std::shared_ptr<RoomServices> services(ErrorCode* error = nullptr) const;
  // Events run serially on the notification task queue, never the transport
  // worker. Register before join. Replacement/removal fences previous callbacks.
  // Callbacks must not issue synchronous control requests, destroy the engine or throw.
  // Serialize registration calls; event references are borrowed for the call.
  // Return promptly. A thrown exception disables the handler and fails the room.
  ErrorCode set_event_handler(std::function<void(const RoomEvent&)>);

 private:
  friend class engine::RtcEngine;
  RoomEngine(TransportFactory, std::shared_ptr<detail::SessionControl>);
  std::shared_ptr<detail::SessionControl> control() const;
  ErrorCode join_locked(RoomOptions options);
  Result<std::shared_ptr<PublishedTrack>> publish_locked(TrackDescriptor descriptor);
  Result<std::shared_ptr<SubscribedTrack>> subscribe_locked(TrackDescriptor remote_track);
  // Waits for the preceding operation, including publication/subscription ACK.
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
  void post_notification(std::function<void()>);
  void notification_failed();
  std::shared_ptr<detail::RoomEngineState> state_;
};

// Restricted borrowed facade. Lifecycle/track mutation goes through RtcEngine.
// Snapshot/services are safe to read across threads. Controls share the session
// operation lock; retained service handles from a departed room stay closed.
class RoomView {
 public:
  RoomSnapshot snapshot() const {
    return room_->snapshot();
  }

  std::shared_ptr<RoomServices> services(ErrorCode* error = nullptr) const {
    return room_->services(error);
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
