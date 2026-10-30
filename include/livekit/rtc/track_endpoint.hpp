#pragma once
#include "livekit/result.hpp"
#include "livekit/rtc/types.hpp"
#include <memory>
#include <optional>
#include <stop_token>

namespace livekit::sdk::rtc {
namespace nodes {
class RtcSubscribeNode;
}

namespace engine {
class RtcEngine;
class TrackGraphRegistry;
class PublishPipeline;
class SubscribePipeline;
}  // namespace engine

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
  friend class engine::RtcEngine;
  friend class engine::TrackGraphRegistry;
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
  friend class engine::RtcEngine;
  friend class engine::TrackGraphRegistry;
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

}  // namespace livekit::sdk::rtc
