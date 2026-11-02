#pragma once

#include "livekit/client/track.hpp"

namespace livekit::sdk::client {
namespace detail {
struct ParticipantData;
struct PublicationData;
}  // namespace detail

class LocalTrackPublication {
 public:
  LocalTrackPublication() = default;

  explicit operator bool() const {
    return bool(data_);
  }

  PublicationSnapshot snapshot() const;
  LocalTrack track() const;

 private:
  friend class detail::RoomSession;
  friend struct detail::ParticipantData;
  friend struct detail::PublicationData;
  friend class LocalParticipant;
  friend class RemoteParticipant;

  explicit LocalTrackPublication(std::shared_ptr<detail::PublicationData> data)
      : data_(std::move(data)) {}

  std::shared_ptr<detail::PublicationData> data_;
};

class RemoteTrackPublication {
 public:
  RemoteTrackPublication() = default;

  explicit operator bool() const {
    return bool(data_);
  }

  PublicationSnapshot snapshot() const;
  void set_subscribed(bool subscribed);
  void set_receive_options(ReceiveOptions options);
  std::optional<RemoteTrack> track() const;

 private:
  friend class detail::RoomSession;
  friend struct detail::ParticipantData;
  friend struct detail::PublicationData;
  friend class LocalParticipant;
  friend class RemoteParticipant;

  explicit RemoteTrackPublication(std::shared_ptr<detail::PublicationData> data)
      : data_(std::move(data)) {}

  std::shared_ptr<detail::PublicationData> data_;
};
}  // namespace livekit::sdk::client
