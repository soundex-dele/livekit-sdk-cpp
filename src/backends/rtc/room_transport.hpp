#pragma once

#include <tuple>
#include "livekit/compat/thread.hpp"
#include "livekit/rtc/types.hpp"
#include "livekit/result.hpp"
#include "support/wake_event.hpp"
#include <functional>

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

  bool operator==(const BindingToken& other) const {
    return std::tie(id, epoch, incarnation, publication) ==
           std::tie(other.id, other.epoch, other.incarnation, other.publication);
  }

  bool operator!=(const BindingToken& other) const {
    return !(*this == other);
  }

  bool operator<(const BindingToken& other) const {
    return std::tie(id, epoch, incarnation, publication) <
           std::tie(other.id, other.epoch, other.incarnation, other.publication);
  }
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
  Failure failure;
};

struct ParticipantUpdateRequest {
  uint32_t id = 0;
  Epoch epoch = 0;
  ParticipantUpdate update;
  SteadyClock::time_point deadline;
  livekit::compat::stop_token stop;
};

struct MuteRequest {
  uint32_t id = 0;
  BindingToken binding;
  bool muted = false;
  SteadyClock::time_point deadline;
  livekit::compat::stop_token stop;
};

struct ReceiveDemandRequest {
  uint32_t id = 0;
  BindingToken binding;
  ReceiveDemand demand;
  SteadyClock::time_point deadline;
  livekit::compat::stop_token stop;
};

struct SubscriptionPermissionsRequest {
  uint32_t id = 0;
  Epoch epoch = 0;
  SubscriptionPermissions permissions;
  SteadyClock::time_point deadline;
  livekit::compat::stop_token stop;
};
enum class SendResult { sent, would_block, dropped };

// Requests initiate backend work. The backend pushes completed events on its
// notification thread, outside its internal locks. stop() fences notifications;
// the owner must outlive stop() and must not stop/destroy itself inside a callback.
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

  virtual Result<void> attach_notifications(Dispatch, std::function<void(Failure)>) {
    return Result<void>::success();
  }

  virtual void set_queue_budget(std::shared_ptr<QueueBudget>) {}

  virtual Result<void> start(const RoomOptions&, Epoch initial_epoch) = 0;

  using Events = std::function<void(std::vector<TransportEvent>)>;

  void set_event_handler(Events handler) {
    events_handler_ = std::move(handler);
  }

  virtual Result<void> publish(const BindingToken&, const TrackDescriptor&) = 0;
  virtual Result<void> subscribe(const BindingToken&, const TrackDescriptor&,
                                 std::shared_ptr<QueueBudget>) = 0;
  // Completion fences native detachment, not a server ACK. Do not wait on the
  // controller: other tracks and control requests must continue to progress.
  using Removed = std::function<void(Failure)>;
  virtual Result<void> remove(const BindingToken&, bool publication, Removed = {}) = 0;
  // would_block requires a later writable event for this exact binding when
  // native capacity returns. Merely waking the room is not a track notification.
  virtual SendResult send(const BindingToken&, const EncodedFrame&) = 0;
  virtual void request_keyframe(const BindingToken&) = 0;

  virtual Result<void> update_participant(ParticipantUpdateRequest request) {
    TransportEvent event{TransportEventKind::control_completed, BindingToken{{}, request.epoch}};
    event.operation_id = request.id;
    event.control_error = ErrorCode::unsupported;
    event.error = "Transport does not support participant updates";
    notify_events({std::move(event)});
    return Result<void>::success();
  }

  virtual Result<void> set_muted(MuteRequest request) {
    TransportEvent event{TransportEventKind::control_completed, request.binding};
    event.operation_id = request.id;
    event.control_error = ErrorCode::unsupported;
    event.error = "Transport does not support publication mute";
    notify_events({std::move(event)});
    return Result<void>::success();
  }

  virtual Result<void> set_receive_demand(ReceiveDemandRequest request) {
    TransportEvent event{TransportEventKind::control_completed, request.binding};
    event.operation_id = request.id;
    event.control_error = ErrorCode::unsupported;
    event.error = "Transport does not support receive demand";
    notify_events({std::move(event)});
    return Result<void>::success();
  }

  virtual Result<void> set_subscription_permissions(SubscriptionPermissionsRequest request) {
    TransportEvent event{TransportEventKind::control_completed, BindingToken{{}, request.epoch}};
    event.operation_id = request.id;
    event.control_error = ErrorCode::unsupported;
    event.error = "Transport does not support subscription permissions";
    notify_events({std::move(event)});
    return Result<void>::success();
  }

  virtual void stop() noexcept = 0;

 protected:
  void notify_events(std::vector<TransportEvent> events) {
    if (events_handler_ && !events.empty()) {
      events_handler_(std::move(events));
    }
  }

 private:
  Events events_handler_;
};

}  // namespace livekit::sdk::rtc::detail
