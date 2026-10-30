#pragma once

#include "livekit/rtc/observer.hpp"
#include "livekit/rtc/room_services.hpp"
#include "livekit/rtc/track_endpoint.hpp"
#include "backends/rtc/room_transport.hpp"
#include <functional>
#include <memory>

namespace livekit::sdk::rtc {
namespace detail {
struct TrackControl;
struct RoomEngineState;
}  // namespace detail

// Internal protocol state machine. No executor, thread affinity or synchronization.
// The owner serializes ALL calls (including reads, polling and destruction).
// Result callbacks run inline after the current protocol batch, never reenter this
// object, and are distinct from ongoing room events. They must not throw.
class RoomEngine {
 public:
  using TransportFactory = std::function<std::unique_ptr<detail::RoomTransport>()>;
  template <class T>
  using Completed = std::function<void(Result<T>)>;

  struct Hooks {
    std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup;
    std::weak_ptr<detail::TrackControl> tracks;
    // Admission/notification storage belongs to the owner. Terminal events must
    // remain observable even when the normal event budget is exhausted.
    std::function<bool(RoomEvent, bool terminal, bool fatal_overflow)> event;
    std::function<void(size_t capacity)> reset_events;
    std::function<void(detail::RoomTransport&)> attach_services;
    // The returned callback is safe on native threads and after room destruction.
    std::function<detail::RoomTransport::Removed(std::shared_ptr<detail::Endpoint>)> removal;
  };

  RoomEngine(TransportFactory, Hooks);
  ~RoomEngine();
  RoomEngine(const RoomEngine&) = delete;
  RoomEngine& operator=(const RoomEngine&) = delete;

  // Synchronously initiate local work; completion reports the operation's actual
  // settlement (ACK, submission, detachment, rejection, timeout or cancellation).
  // Even immediate failures use the operation callback; there is no second result.
  void join(RoomOptions, Completed<void>);
  void publish(TrackDescriptor, Completed<std::shared_ptr<PublishedTrack>>);
  void subscribe(TrackDescriptor, Completed<std::shared_ptr<SubscribedTrack>>);
  void leave(Completed<void>);
  void unpublish(const TrackId&, Completed<void>);
  void unsubscribe(const TrackId&, Completed<void>);
  void set_muted(const TrackId&, bool, ControlOptions, Completed<void>,
                 std::shared_ptr<detail::Endpoint> expected = {});
  void set_receive_demand(const TrackId&, ReceiveDemand, ControlOptions, Completed<void>,
                          std::shared_ptr<detail::Endpoint> expected = {});
  void update_local_participant(ParticipantUpdate, ControlOptions, Completed<void>);
  void set_subscription_permissions(SubscriptionPermissions, ControlOptions, Completed<void>);
  void close_track(std::shared_ptr<detail::Endpoint>, Completed<void>);
  void request_keyframe(std::shared_ptr<detail::Endpoint>, Completed<void>);

  // Owner-driven servicing; no waiting. Input notifications merely wake the owner.
  void poll(bool interrupted = false);
  std::optional<SteadyClock::time_point> next_deadline() const;
  void report_failure(std::shared_ptr<detail::Endpoint>, std::exception_ptr);
  void finish_removal(std::shared_ptr<detail::Endpoint>, std::exception_ptr);
  void notification_failed(std::exception_ptr);
  RoomSnapshot snapshot() const;
  Result<std::shared_ptr<RoomServices>> services() const;

 private:
  std::unique_ptr<detail::RoomEngineState> state_;
};
}  // namespace livekit::sdk::rtc
