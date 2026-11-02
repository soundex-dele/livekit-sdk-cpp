#pragma once

#include "livekit/client/track_publication.hpp"

namespace livekit::sdk::client {
namespace detail {
struct ParticipantData;
}  // namespace detail

class LocalParticipant {
 public:
  LocalParticipant() = default;

  explicit operator bool() const {
    return bool(data_);
  }

  ParticipantSnapshot snapshot() const;
  std::vector<LocalTrackPublication> track_publications() const;
  Result<LocalTrackPublication> publish_track(LocalTrack track, TrackPublishOptions options = {});
  void unpublish_track(TrackSid sid);
  void publish_data(DataPacket packet);
  void update(ParticipantUpdate update);

 private:
  friend class detail::RoomSession;

  explicit LocalParticipant(std::shared_ptr<detail::ParticipantData> data)
      : data_(std::move(data)) {}

  std::shared_ptr<detail::ParticipantData> data_;
};

class RemoteParticipant {
 public:
  RemoteParticipant() = default;

  explicit operator bool() const {
    return bool(data_);
  }

  ParticipantSnapshot snapshot() const;
  std::vector<RemoteTrackPublication> track_publications() const;

 private:
  friend class detail::RoomSession;

  explicit RemoteParticipant(std::shared_ptr<detail::ParticipantData> data)
      : data_(std::move(data)) {}

  std::shared_ptr<detail::ParticipantData> data_;
};
}  // namespace livekit::sdk::client
