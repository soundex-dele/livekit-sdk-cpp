#pragma once
#include "livekit/rtc/completion.hpp"
#include "livekit/rtc/room_services.hpp"
#include "livekit/rtc/track_endpoint.hpp"

namespace livekit::sdk::rtc {
// Borrowed until RtcEngine destruction. Reads are synchronized by the engine;
// mutations use its admission, cancellation and notification contract.
class RoomView {
 public:
  RoomSnapshot snapshot() const;
  Result<std::shared_ptr<RoomServices>> services() const;
  [[nodiscard]] ErrorCode update_local_participant(ParticipantUpdate, Completion<void>,
                                                   ControlOptions = {}) const;
  [[nodiscard]] ErrorCode set_subscription_permissions(SubscriptionPermissions, Completion<void>,
                                                       ControlOptions = {}) const;

 private:
  friend class engine::RtcEngine;

  explicit RoomView(engine::RtcEngine* engine) : engine_(engine) {}

  engine::RtcEngine* engine_;
};
}  // namespace livekit::sdk::rtc
