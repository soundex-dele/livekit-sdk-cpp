#pragma once
#include "livekit/rtc/observer.hpp"
#include "livekit/rtc/room_services.hpp"
#include <functional>
#include <memory>
#include <map>
#include <vector>
#include <utility>
#include <cstdint>
#include <optional>

namespace livekit::sdk::rtc {
namespace detail {
struct BindingToken;
struct TransportEvent;
struct TrackControl;
struct Endpoint;
struct QueueBudget;
class RoomTransport;
}  // namespace detail

// Single-room protocol owner. Operations initiate work and return immediately;
// successful completion is reported through the observer. No locking, waiting,
// exception translation, timeout/retry or reconnect restoration is provided.
class RoomEngine {
 public:
  using TransportFactory = std::function<std::unique_ptr<detail::RoomTransport>()>;
  explicit RoomEngine(TransportFactory);
  ~RoomEngine();
  RoomEngine(const RoomEngine&) = delete;
  RoomEngine& operator=(const RoomEngine&) = delete;

  void set_observer(RoomObserver*);
  void join(RoomOptions);
  void leave();
  void publish(TrackDescriptor);
  void subscribe(TrackDescriptor);
  void subscribe(const TrackId&);
  void unpublish(const TrackId&);
  void unsubscribe(const TrackId&);
  void set_muted(const TrackId&, bool);
  void set_receive_demand(const TrackId&, ReceiveDemand);
  void update_local_participant(ParticipantUpdate);
  void set_subscription_permissions(SubscriptionPermissions);

  RoomSnapshot snapshot() const;
  std::shared_ptr<RoomServices> services() const;

 private:
  struct Entry {
    std::shared_ptr<detail::Endpoint> endpoint;
    std::shared_ptr<PublishedTrack> published;
    std::shared_ptr<SubscribedTrack> subscribed;
    bool removing = false;
    std::optional<uint64_t> keyframe_requests;
  };

  void event(RoomEvent);
  void on_events(std::vector<detail::TransportEvent>);
  std::shared_ptr<detail::Endpoint> make_endpoint(TrackDescriptor, bool publication);
  void send(const std::shared_ptr<detail::Endpoint>&);
  void request_keyframe(const std::shared_ptr<detail::Endpoint>&);
  void remove(const TrackId&, bool publication);
  void removed(const detail::BindingToken&);

  TransportFactory factory_;
  std::unique_ptr<detail::RoomTransport> transport_;
  RoomObserver* observer_ = nullptr;
  RoomSnapshot status_;
  std::shared_ptr<detail::QueueBudget> budget_;
  std::shared_ptr<detail::TrackControl> commands_;
  std::map<std::pair<TrackId, bool>, Entry> tracks_;
  std::map<TrackId, TrackDescriptor> remote_tracks_;
  uint64_t next_binding_ = 0;
  uint32_t next_control_ = 0;
};
}  // namespace livekit::sdk::rtc
