#pragma once

#include "livekit/client/participant.hpp"

namespace livekit::sdk::client {
enum class RoomEventKind {
  connection_changed,
  participant_connected,
  participant_disconnected,
  participant_changed,
  track_published,
  track_unpublished,
  track_subscribed,
  track_unsubscribed,
  track_failed,
  track_republished,
  track_muted,
  sender_feedback,
  data_received
};

// Owned snapshot values may be retained after the callback returns.
struct RoomEvent {
  RoomEventKind kind = RoomEventKind::connection_changed;
  ConnectionState connection = ConnectionState::created;
  Epoch epoch = 0;
  std::string participant_identity;
  std::optional<ParticipantSnapshot> participant;
  std::optional<PublicationSnapshot> publication;
  TrackSid previous_sid;
  std::optional<SenderFeedback> feedback;
  std::optional<DataPacket> data;
  Failure failure;
};
}  // namespace livekit::sdk::client
