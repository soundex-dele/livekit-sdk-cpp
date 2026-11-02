#pragma once
#include "livekit/client/room.hpp"
#include "backends/livekit/participant_state.hpp"

namespace livekit::sdk::client::detail {
// Room coordinates roster and events; operations live with their domain objects.
class RoomSession : public std::enable_shared_from_this<RoomSession> {
 public:
  explicit RoomSession(SessionFactory = create_rtc_session);
  ~RoomSession();
  void observer(RoomObserver*);
  bool connect(ConnectionOptions);
  void close();
  ConnectionState state() const;
  RoomInfo info() const;
  LocalParticipant local() const;
  std::vector<RemoteParticipant> remotes() const;

 private:
  Result<void> on_join(const JoinState&, bool, RtcSession&, const Operation&);
  void on_event(SessionEvent);
  void on_state(ConnectionState, Failure);
  void participant(ParticipantSnapshot);
  void notify(RoomEvent);
  void run_notifications();
  std::shared_ptr<RtcEngine> engine_;
  mutable std::mutex mutex_;
  ConnectionState state_ = ConnectionState::created;
  RoomInfo info_;
  std::shared_ptr<ParticipantData> local_;
  std::map<std::string, std::shared_ptr<ParticipantData>> remote_;
  std::shared_ptr<QueueBudget> budget_;
  ConnectionOptions options_;

  struct SubscriptionIntent {
    TrackSid sid;
    bool desired;
    ReceiveOptions receive;
  };

  using SubscriptionKey = std::tuple<std::string, std::string, MediaKind, PublicationSource>;
  std::map<SubscriptionKey, std::vector<SubscriptionIntent>> subscription_intents_;
  std::mutex observer_change_, notification_mutex_;
  std::condition_variable notification_changed_;
  std::deque<RoomEvent> notifications_;
  RoomObserver* observer_ = nullptr;
  bool notifying_ = false, notifications_closed_ = false;
  size_t notification_bytes_ = 0;
  std::atomic<size_t> max_events_{4096};
  std::thread notifier_;
};
}  // namespace livekit::sdk::client::detail
