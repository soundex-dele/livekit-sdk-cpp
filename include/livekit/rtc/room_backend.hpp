#pragma once

#include "livekit/rtc/types.hpp"
#include "livekit/rtc/room_services.hpp"
#include <future>
#include <stop_token>

namespace livekit::sdk::rtc {
namespace detail { class RoomTransport; struct Endpoint; struct BackendState; }
struct ControlOptions {
  std::chrono::milliseconds timeout{0}; // Zero uses RoomOptions::operation_timeout.
  std::stop_token stop;
};

class PublishedTrack {
 public:
  const TrackDescriptor& descriptor() const;
  TrackSnapshot snapshot() const;
  SenderFeedback feedback() const;
  WriteStatus try_write(EncodedFrame frame);
  // Local gate plus signaling submission, not a server ACK. Already transmitted
  // packets cannot be retracted. Muting does not unpublish the track.
  std::future<void> set_muted(bool muted, ControlOptions = {});
  // Completes after local transport detachment; it does not disconnect the room.
  std::future<void> close();
 private:
  friend class RoomBackend;
  PublishedTrack(std::shared_ptr<detail::Endpoint>, std::weak_ptr<detail::BackendState>);
  std::shared_ptr<detail::Endpoint> endpoint_;
  std::weak_ptr<detail::BackendState> backend_;
};

class SubscribedTrack {
 public:
  const TrackDescriptor& descriptor() const;
  TrackSnapshot snapshot() const;
  ReadResult read(std::chrono::milliseconds timeout, std::stop_token stop = {});
  // Sends a rate-limited PLI independently of incoming media arrival.
  std::future<void> request_keyframe();
  // SFU forwarding demand, not a local decoder/device gate. Success means
  // signaling submission; in-flight media can still arrive. Full replacement.
  std::future<void> set_demand(ReceiveDemand, ControlOptions = {});
  std::future<void> close();
 private:
  friend class RoomBackend;
  SubscribedTrack(std::shared_ptr<detail::Endpoint>, std::weak_ptr<detail::BackendState>);
  std::shared_ptr<detail::Endpoint> endpoint_;
  std::weak_ptr<detail::BackendState> backend_;
};

// Transport-only room owner. The private adapter is injected so the same state
// machine can be tested without network, devices or codecs. The native factory
// provides this adapter; callers do not implement GraphEngine or WebRTC objects.
class RoomBackend {
 public:
  explicit RoomBackend(std::unique_ptr<detail::RoomTransport> transport);
  ~RoomBackend();
  RoomBackend(const RoomBackend&) = delete;
  RoomBackend& operator=(const RoomBackend&) = delete;

  std::future<void> join(RoomOptions options);
  std::future<std::shared_ptr<PublishedTrack>> publish(TrackDescriptor descriptor);
  std::future<std::shared_ptr<SubscribedTrack>> subscribe(TrackDescriptor remote_track);
  // Also cancels an operation still waiting for publication/subscription ACK.
  std::future<void> unpublish(const TrackId& id);
  std::future<void> unsubscribe(const TrackId& id);
  std::future<void> set_muted(const TrackId& id, bool muted, ControlOptions = {});
  std::future<void> set_receive_demand(const TrackId& id, ReceiveDemand, ControlOptions = {});
  // Replaces the publisher's allowlist. Success means signaling submission,
  // not server acknowledgement. Native policy intent survives rejoin even if
  // an interrupted submission has an unknown outcome.
  std::future<void> set_subscription_permissions(SubscriptionPermissions, ControlOptions = {});
  // Requires signaling protocol 15+. Resolves only on the server's ACK, not
  // after a local send. A dispatched operation interrupted/cancelled/timed out
  // can have an unknown outcome and is never automatically replayed.
  std::future<void> update_local_participant(ParticipantUpdate, ControlOptions = {});
  std::future<void> leave();
  RoomSnapshot snapshot() const;
  std::shared_ptr<RoomServices> services() const;
  std::optional<RoomEvent> next_event(std::chrono::milliseconds timeout, std::stop_token stop = {});
 private:
  std::shared_ptr<detail::BackendState> state_;
};

} // namespace livekit::sdk::rtc
