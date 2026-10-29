#pragma once

#include "livekit/rtc/types.hpp"
#include "livekit/error.hpp"
#include <stop_token>
#include <exception>

namespace livekit::sdk::rtc { class RoomServices; }
namespace livekit::sdk::rtc::detail {

// Echoed by every track event. An id alone cannot distinguish a cancelled
// publication, a new publication with the same id, and a restored binding.
struct BindingToken {
  TrackId id;
  Epoch epoch = 0;
  uint64_t incarnation = 0;
  bool publication = true;
  bool operator==(const BindingToken&) const = default;
};

enum class TransportEventKind {
  connected, interrupted, terminal_error,
  published, subscribed, removed, operation_failed,
  frame, feedback, remote_track_added, remote_track_removed, encryption_state_changed, session_changed, control_completed, mute_changed, subscription_permission_changed, statistics
};
struct TransportEvent {
  TransportEventKind kind;
  BindingToken binding;
  std::string server_sid;
  std::string error;
  std::optional<TrackDescriptor> descriptor;
  std::optional<EncodedFrame> frame;
  std::optional<SenderFeedback> feedback;
  std::optional<EncryptionStateEvent> encryption;
  std::shared_ptr<const SessionInfo> session;
  uint32_t operation_id = 0;
  std::optional<ErrorCode> control_error;
  int control_reason = 0;
  bool muted = false;
  std::optional<SubscriptionPermissionChange> permission;
  std::shared_ptr<const TransportStatistics> statistics;
  std::exception_ptr failure;
};
struct ParticipantUpdateRequest {
  uint32_t id = 0;
  Epoch epoch = 0;
  ParticipantUpdate update;
  SteadyClock::time_point deadline;
  std::stop_token stop;
};
struct MuteRequest {
  uint32_t id = 0;
  BindingToken binding;
  bool muted = false;
  SteadyClock::time_point deadline;
  std::stop_token stop;
};
struct ReceiveDemandRequest {
  uint32_t id = 0;
  BindingToken binding;
  ReceiveDemand demand;
  SteadyClock::time_point deadline;
  std::stop_token stop;
};
struct SubscriptionPermissionsRequest {
  uint32_t id = 0;
  Epoch epoch = 0;
  SubscriptionPermissions permissions;
  SteadyClock::time_point deadline;
  std::stop_token stop;
};
enum class SendResult { sent, would_block, dropped };

// All methods execute on the RoomBackend controller, never on a graph thread.
// start/publish/subscribe enqueue work and must not wait for a server response.
// poll collects native callbacks and drives signaling. Native threads never
// enter application code. Connection recovery is adapter-owned; restoring the
// desired publication/subscription registry is exclusively BackendState-owned.
class RoomTransport {
 public:
  virtual ~RoomTransport() = default;
  virtual std::shared_ptr<RoomServices> services() const { return {}; }
  virtual void set_queue_budget(std::shared_ptr<QueueBudget>) {}
  virtual void start(const RoomOptions&) = 0;
  virtual std::vector<TransportEvent> poll(std::chrono::milliseconds wait, std::stop_token) = 0;
  virtual void publish(const BindingToken&, const TrackDescriptor&) = 0;
  virtual void subscribe(const BindingToken&, const TrackDescriptor&, std::shared_ptr<QueueBudget>) = 0;
  virtual void remove(const BindingToken&, bool publication) = 0;
  virtual SendResult send(const BindingToken&, const EncodedFrame&) = 0;
  virtual void request_keyframe(const BindingToken&) = 0;
  virtual void update_participant(ParticipantUpdateRequest) {
    throw SDKError(ErrorDomain::protocol, ErrorCode::unsupported, "Transport does not support participant updates");
  }
  virtual void set_muted(MuteRequest) {
    throw SDKError(ErrorDomain::protocol, ErrorCode::unsupported, "Transport does not support publication mute");
  }
  virtual void set_receive_demand(ReceiveDemandRequest) {
    throw SDKError(ErrorDomain::protocol, ErrorCode::unsupported, "Transport does not support receive demand");
  }
  virtual void set_subscription_permissions(SubscriptionPermissionsRequest) {
    throw SDKError(ErrorDomain::protocol, ErrorCode::unsupported, "Transport does not support subscription permissions");
  }
  virtual void stop() noexcept = 0;
};

} // namespace livekit::sdk::rtc::detail
