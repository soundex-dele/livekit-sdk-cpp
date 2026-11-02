#pragma once
#include "backends/livekit/track_state.hpp"

namespace livekit::sdk::client::detail {
struct PublicationData {
  mutable std::mutex mutex;
  PublicationSnapshot info;
  TrackPublishOptions options;
  ReceiveOptions receive;
  std::shared_ptr<TrackData> track;
  std::weak_ptr<RtcEngine> engine;
  std::function<void(RoomEvent)> notify;
  Result<void> subscribe(RtcSession&, const Operation&, bool);
  void emit(RoomEventKind, Failure = {}, TrackSid previous = {});
};

struct ParticipantData {
  mutable std::mutex mutex;
  ParticipantSnapshot info;
  std::map<TrackId, std::shared_ptr<PublicationData>> publications;
  std::weak_ptr<RtcEngine> engine;
  std::shared_ptr<QueueBudget> budget;
  std::function<void(RoomEvent)> notify;
  Result<LocalTrackPublication> publish(std::shared_ptr<TrackData>, TrackPublishOptions,
                                        RtcSession&, const Operation&);
  Result<void> unpublish(const TrackSid&, RtcSession&, const Operation&);
  Result<void> restore(RtcSession&, const Operation&);
  void suspend();
  void close();
  void on_event(const SessionEvent&);
};
}  // namespace livekit::sdk::client::detail
