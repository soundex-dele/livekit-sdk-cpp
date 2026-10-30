#pragma once

#include "livekit/rtc/types.hpp"
#include "livekit/error.hpp"
#include "support/wake_event.hpp"
#include <stop_token>
#include <exception>
#include <future>
#include <functional>
#include <compare>

namespace livekit::sdk::rtc {
class RoomServices;
}

namespace livekit::sdk::rtc::detail {

// Echoed by every track event. An id alone cannot distinguish a cancelled
// publication, a new publication with the same id, and a restored binding.
struct BindingToken {
  TrackId id;
  Epoch epoch = 0;
  uint64_t incarnation = 0;
  bool publication = true;
  bool operator==(const BindingToken&) const = default;
  auto operator<=>(const BindingToken&) const = default;
};

enum class TransportEventKind {
  connected,
  interrupted,
  terminal_error,
  published,
  subscribed,
  removed,
  operation_failed,
  frame,
  feedback,
  writable,
  remote_track_added,
  remote_track_removed,
  encryption_state_changed,
  session_changed,
  control_completed,
  mute_changed,
  subscription_permission_changed,
  statistics
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

// Calls are serialized by RoomEngine. Control requests run on its control
// worker; idle event/media servicing and recovery run on its transport worker.
// Adapters must not depend on a fixed controller thread. Never called concurrently.
// start/publish/subscribe enqueue work and must not wait for a server response.
// take_events drains callbacks after a wakeup; it never waits. Native threads never
// enter application code. Connection recovery is adapter-owned; restoring the
// desired publication/subscription registry is exclusively RoomEngineState-owned.
class RoomTransport {
 public:
  virtual ~RoomTransport() = default;

  virtual BackendCapabilities capabilities() const {
    return {};
  }

  virtual std::shared_ptr<RoomServices> services() const {
    return {};
  }

  // Borrowed, lifetime-fenced room notification lane. The failure reporter
  // retains no room/adapter pointer and remains safe after transport shutdown.
  using Dispatch = std::function<void(std::function<void()>)>;

  virtual void attach_notifications(Dispatch, std::function<void(std::exception_ptr)>) {}

  virtual void set_queue_budget(std::shared_ptr<QueueBudget>) {}

  virtual void start(const RoomOptions&, Epoch initial_epoch) = 0;

  void attach_wakeup(std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup) {
    wakeup_ = std::move(wakeup);
  }

  virtual std::vector<TransportEvent> take_events() = 0;
  virtual void publish(const BindingToken&, const TrackDescriptor&) = 0;
  virtual void subscribe(const BindingToken&, const TrackDescriptor&,
                         std::shared_ptr<QueueBudget>) = 0;
  // Completion fences native detachment, not a server ACK. Do not wait on the
  // controller: other tracks and control requests must continue to progress.
  using Removed = std::function<void(std::exception_ptr)>;
  virtual void remove(const BindingToken&, bool publication, Removed = {}) = 0;
  // would_block requires a later writable event for this exact binding when
  // native capacity returns. Merely waking the room is not a track notification.
  virtual SendResult send(const BindingToken&, const EncodedFrame&) = 0;
  virtual void request_keyframe(const BindingToken&) = 0;

  virtual void update_participant(ParticipantUpdateRequest) {
    throw SDKError(ErrorDomain::protocol, ErrorCode::unsupported,
                   "Transport does not support participant updates");
  }

  virtual void set_muted(MuteRequest) {
    throw SDKError(ErrorDomain::protocol, ErrorCode::unsupported,
                   "Transport does not support publication mute");
  }

  virtual void set_receive_demand(ReceiveDemandRequest) {
    throw SDKError(ErrorDomain::protocol, ErrorCode::unsupported,
                   "Transport does not support receive demand");
  }

  virtual void set_subscription_permissions(SubscriptionPermissionsRequest) {
    throw SDKError(ErrorDomain::protocol, ErrorCode::unsupported,
                   "Transport does not support subscription permissions");
  }

  virtual void stop() noexcept = 0;

 protected:
  std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup_;
};

}  // namespace livekit::sdk::rtc::detail
