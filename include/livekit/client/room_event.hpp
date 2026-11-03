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
  data_received,
  dtmf_received,
  transcription_received,
  encryption_changed,
  data_error,
  data_tracks_changed
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
  std::optional<DtmfDigit> dtmf;
  std::optional<Transcription> transcription;
  std::optional<EncryptionStateEvent> encryption;
  Failure failure;
};
}  // namespace livekit::sdk::client
