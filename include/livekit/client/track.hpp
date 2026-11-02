#pragma once

#include "livekit/client/types.hpp"

namespace livekit::sdk::client {
namespace detail {
struct ParticipantData;
struct PublicationData;
struct TrackData;
class RoomSession;
}  // namespace detail

// Copyable media handle. Creation does not capture, encode, or publish media.
class LocalTrack {
 public:
  LocalTrack() = default;
  static Result<LocalTrack> create_audio(std::string name, CodecFormat format,
                                         QueueLimits limits = {});
  static Result<LocalTrack> create_video(std::string name, CodecFormat format,
                                         QueueLimits limits = {});

  explicit operator bool() const {
    return bool(data_);
  }

  TrackSnapshot snapshot() const;
  SenderFeedback feedback() const;
  WriteStatus try_write(EncodedFrame frame);
  void set_muted(bool muted);

 private:
  friend class detail::RoomSession;
  friend struct detail::ParticipantData;
  friend struct detail::PublicationData;
  friend class LocalParticipant;
  friend class RemoteParticipant;
  friend class LocalTrackPublication;

  explicit LocalTrack(std::shared_ptr<detail::TrackData> data) : data_(std::move(data)) {}

  static Result<LocalTrack> create(std::string, CodecFormat, QueueLimits, MediaKind);
  std::shared_ptr<detail::TrackData> data_;
};

// Single-consumer encoded receive handle. Copies share the same frame queue.
class RemoteTrack {
 public:
  RemoteTrack() = default;

  explicit operator bool() const {
    return bool(data_);
  }

  TrackSnapshot snapshot() const;
  ReadResult read(WaitOptions options = {});
  void request_keyframe();

 private:
  friend class RemoteTrackPublication;

  explicit RemoteTrack(std::shared_ptr<detail::TrackData> data) : data_(std::move(data)) {}

  std::shared_ptr<detail::TrackData> data_;
};

// Audio/video kinds are validated at creation; uniform handles avoid slicing.
using LocalAudioTrack = LocalTrack;
using LocalVideoTrack = LocalTrack;
using RemoteAudioTrack = RemoteTrack;
using RemoteVideoTrack = RemoteTrack;
}  // namespace livekit::sdk::client
