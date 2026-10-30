#pragma once
#include "livekit/result.hpp"

#include "livekit/rtc/types.hpp"
#include "livekit/rtc/room_services.hpp"
#include <future>
#include <functional>
#include <stop_token>
#include <memory>
#include <optional>
#include <utility>

namespace livekit::sdk::rtc {
namespace nodes {
class RtcSubscribeNode;
}
namespace engine {
class RtcEngine;
class RtcPipelineEngine;
class PublishPipeline;
class SubscribePipeline;
}

namespace detail {
class RoomTransport;
struct Endpoint;
struct RoomEngineState;
struct SessionControl;
}  // namespace detail

struct ControlOptions {
  std::chrono::milliseconds timeout{0};  // Zero uses RoomOptions::operation_timeout.
  std::stop_token stop;
};

class PublishedTrack {
 public:
  const TrackDescriptor& descriptor() const;
  TrackSnapshot snapshot() const;
  SenderFeedback feedback() const;
  // Wait for a new epoch/revision without polling. Null on stop or terminal
  // endpoint state. Multiple observers may independently retain their cursors.
  std::optional<SenderFeedback> wait_feedback(Epoch previous_epoch, uint64_t previous_revision,
                                              std::stop_token stop = {}) const;
  WriteStatus try_write(EncodedFrame frame);
  // Local gate plus signaling submission, not a server ACK. Already transmitted
  // packets cannot be retracted. Muting does not unpublish the track.
  Result<void> set_muted(bool muted, ControlOptions = {});
  // Completes after local transport detachment; it does not disconnect the room.
  Result<void> close();

 private:
  friend class RoomEngine;
  friend class engine::RtcPipelineEngine;
  friend class engine::PublishPipeline;
  std::shared_ptr<detail::SessionControl> operation_control() const;
  Result<void> close_locked();
  void report_failure(std::exception_ptr);
  PublishedTrack(std::shared_ptr<detail::Endpoint>, std::weak_ptr<detail::RoomEngineState>);
  std::shared_ptr<detail::Endpoint> endpoint_;
  std::weak_ptr<detail::RoomEngineState> session_;
};

class SubscribedTrack {
 public:
  const TrackDescriptor& descriptor() const;
  TrackSnapshot snapshot() const;
  ReadResult read(std::chrono::milliseconds timeout, std::stop_token stop = {});
  // Single-reader blocking receive. Remains asleep across suspension/rebind;
  // returns on media, cancellation, failure or close. No timeout polling.
  ReadResult wait_frame(std::stop_token stop = {});
  // Sends a rate-limited PLI independently of incoming media arrival.
  Result<void> request_keyframe();
  // SFU forwarding demand, not a local decoder/device gate. Success means
  // signaling submission; in-flight media can still arrive. Full replacement.
  Result<void> set_demand(ReceiveDemand, ControlOptions = {});
  Result<void> close();

 private:
  friend class RoomEngine;
  friend class engine::RtcPipelineEngine;
  friend class engine::SubscribePipeline;
  friend class nodes::RtcSubscribeNode;
  void request_keyframe_async();
  std::shared_ptr<detail::SessionControl> operation_control() const;
  Result<void> close_locked();
  void report_failure(std::exception_ptr);
  SubscribedTrack(std::shared_ptr<detail::Endpoint>, std::weak_ptr<detail::RoomEngineState>);
  std::shared_ptr<detail::Endpoint> endpoint_;
  std::weak_ptr<detail::RoomEngineState> session_;
};

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

  Result<void> join(RoomOptions options);
  Result<std::shared_ptr<PublishedTrack>> publish(TrackDescriptor descriptor);
  Result<std::shared_ptr<SubscribedTrack>> subscribe(TrackDescriptor remote_track);
  // Waits for the preceding operation, including publication/subscription ACK.
  Result<void> unpublish(const TrackId& id);
  Result<void> unsubscribe(const TrackId& id);
  Result<void> set_muted(const TrackId& id, bool muted, ControlOptions = {});
  Result<void> set_receive_demand(const TrackId& id, ReceiveDemand, ControlOptions = {});
  // Replaces the publisher's allowlist. Success means signaling submission,
  // not server acknowledgement. Native policy intent survives rejoin even if
  // an interrupted submission has an unknown outcome.
  Result<void> set_subscription_permissions(SubscriptionPermissions, ControlOptions = {});
  // Requires signaling protocol 15+. Resolves only on the server's ACK, not
  // after a local send. A dispatched operation interrupted/cancelled/timed out
  // can have an unknown outcome and is never automatically replayed.
  Result<void> update_local_participant(ParticipantUpdate, ControlOptions = {});
  Result<void> leave();
  RoomSnapshot snapshot() const;
  Result<std::shared_ptr<RoomServices>> services() const;
  // Events run serially on the notification task queue, never the transport
  // worker. Register before join. Replacement/removal fences previous callbacks.
  // Callbacks must not issue synchronous control requests, destroy the engine or throw.
  // Serialize registration calls; event references are borrowed for the call.
  // Return promptly. A thrown exception disables the handler and fails the room.
  Result<void> set_event_handler(std::function<void(const RoomEvent&)>);

 private:
  friend class engine::RtcEngine;
  friend class engine::RtcPipelineEngine;
  detail::SessionControl& control() const;
  Result<void> join_locked(RoomOptions options);
  Result<std::shared_ptr<PublishedTrack>> publish_locked(TrackDescriptor descriptor);
  Result<std::shared_ptr<SubscribedTrack>> subscribe_locked(TrackDescriptor remote_track);
  // Waits for the preceding operation, including publication/subscription ACK.
  Result<void> unpublish_locked(const TrackId& id);
  Result<void> unsubscribe_locked(const TrackId& id);
  Result<void> set_muted_locked(const TrackId& id, bool muted, ControlOptions = {});
  Result<void> set_receive_demand_locked(const TrackId& id, ReceiveDemand, ControlOptions = {});
  // Replaces the publisher's allowlist. Success means signaling submission,
  // not server acknowledgement. Native policy intent survives rejoin even if
  // an interrupted submission has an unknown outcome.
  Result<void> set_subscription_permissions_locked(SubscriptionPermissions, ControlOptions = {});
  // Requires signaling protocol 15+. Resolves only on the server's ACK, not
  // after a local send. A dispatched operation interrupted/cancelled/timed out
  // can have an unknown outcome and is never automatically replayed.
  Result<void> update_local_participant_locked(ParticipantUpdate, ControlOptions = {});
  Result<void> leave_locked();
  void set_event_handler_locked(std::function<void(const RoomEvent&)>);
  void post_notification(std::function<void()>);
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

  Result<std::shared_ptr<RoomServices>> services() const {
    return room_->services();
  }

  Result<void> update_local_participant(ParticipantUpdate update,
                                        ControlOptions options = {}) const {
    return room_->update_local_participant(std::move(update), options);
  }

  Result<void> set_subscription_permissions(SubscriptionPermissions policy,
                                            ControlOptions options = {}) const {
    return room_->set_subscription_permissions(std::move(policy), options);
  }

 private:
  friend class engine::RtcEngine;

  explicit RoomView(RoomEngine* room) : room_(room) {}

  RoomEngine* room_;
};
}  // namespace livekit::sdk::rtc
