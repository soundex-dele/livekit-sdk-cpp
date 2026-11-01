#pragma once
#include "livekit/rtc/observer.hpp"
#include "livekit/rtc/completion.hpp"
#include "livekit/rtc/room_services.hpp"
#include <functional>
#include <memory>
#include <map>
#include <vector>
#include <utility>
#include <cstdint>
#include <optional>
#include <mutex>
#include <atomic>

namespace livekit::sdk::rtc {
namespace detail {
struct BindingToken;
struct TransportEvent;
struct TrackControl;
struct Endpoint;
struct QueueBudget;
class RoomTransport;
struct RoomExecution;
}  // namespace detail

// Room state and transport requests run on control. All public completions and
// observers run on notification. No media lifecycle worker or graph is created.
// Requests are posted; results arrive through Completion. Dependent operations
// should be chained from completion. leave() and observer replacement are
// synchronous barriers and require an external owner thread.
class RoomEngine {
 public:
  using TransportFactory = std::function<std::unique_ptr<detail::RoomTransport>()>;
  explicit RoomEngine(TransportFactory);
  ~RoomEngine();
  RoomEngine(const RoomEngine&) = delete;
  RoomEngine& operator=(const RoomEngine&) = delete;

  [[nodiscard]] ErrorCode set_observer(RoomObserver*);
  void join(RoomOptions, Completion<RoomSnapshot> = {});
  void leave(Completion<void> = {});
  void publish(TrackDescriptor, Completion<std::shared_ptr<PublishedTrack>> = {});
  void subscribe(TrackDescriptor, Completion<std::shared_ptr<SubscribedTrack>> = {});
  void subscribe(const TrackId&, Completion<std::shared_ptr<SubscribedTrack>> = {});
  void unpublish(const TrackId&, Completion<void> = {});
  void unsubscribe(const TrackId&, Completion<void> = {});
  void set_muted(const TrackId&, bool, Completion<void> = {});
  void set_receive_demand(const TrackId&, ReceiveDemand, Completion<void> = {});
  void update_local_participant(ParticipantUpdate, Completion<void> = {});
  void set_subscription_permissions(SubscriptionPermissions, Completion<void> = {});

  RoomSnapshot snapshot() const;
  std::shared_ptr<RoomServices> services() const;

 private:
  friend class engine::RtcEngine;
  void notify(std::function<void(RoomObserver&)>);
  void deliver(std::function<void()>);
  template <class T, class F>
  void post_request(Completion<T>, F);

  template <class T>
  void complete(Completion<T>& callback, Result<T> result) {
    auto done = std::exchange(callback, {});
    if (done) {
      deliver([done = std::move(done), result = std::move(result)]() mutable {
        done(std::move(result));
      });
    }
  }

  void fail_pending(std::shared_ptr<const FailureInfo>);

  struct Entry {
    std::shared_ptr<detail::Endpoint> endpoint;
    std::shared_ptr<PublishedTrack> published;
    std::shared_ptr<SubscribedTrack> subscribed;
    Completion<std::shared_ptr<PublishedTrack>> publish_done;
    Completion<std::shared_ptr<SubscribedTrack>> subscribe_done;
    std::vector<Completion<void>> remove_done;
    bool removing = false;
    std::optional<uint64_t> keyframe_requests;
  };

  void event(RoomEvent);
  void on_events(std::vector<detail::TransportEvent>);
  std::shared_ptr<detail::Endpoint> make_endpoint(TrackDescriptor, bool publication);
  void send(const std::shared_ptr<detail::Endpoint>&);
  void request_keyframe(const std::shared_ptr<detail::Endpoint>&);
  void remove(const TrackId&, bool publication, Completion<void> = {});
  void removed(const detail::BindingToken&);
  void subscribe_on_control(TrackDescriptor, Completion<std::shared_ptr<SubscribedTrack>>);

  std::shared_ptr<detail::RoomExecution> execution_;
  std::atomic<bool> closing_{false};
  std::mutex leave_mutex_;
  std::mutex request_mutex_;  // admission only; never held while waiting for a worker
  TransportFactory factory_;
  std::unique_ptr<detail::RoomTransport> transport_;
  RoomObserver* observer_ = nullptr;  // control worker
  RoomSnapshot status_;
  std::shared_ptr<detail::QueueBudget> budget_;
  std::shared_ptr<detail::TrackControl> commands_;
  std::map<std::pair<TrackId, bool>, Entry> tracks_;
  std::map<TrackId, TrackDescriptor> remote_tracks_;
  Completion<RoomSnapshot> join_completion_;  // control worker

  struct ControlCompletion {
    Epoch epoch;
    TrackId track;
    bool publication;
    Completion<void> callback;
  };

  std::map<uint32_t, ControlCompletion> control_completions_;
  uint64_t next_binding_ = 0;
  uint32_t next_control_ = 0;
};

// Requires LiveKit::RTCLiveKit, without GraphEngine or SDK capture/playout targets.
std::unique_ptr<RoomEngine> create_livekit_room();
}  // namespace livekit::sdk::rtc
