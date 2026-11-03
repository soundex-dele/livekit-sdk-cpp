#pragma once
#include "livekit/client/room.hpp"
#include "livekit/rtc/media_config.hpp"

namespace livekit::sdk::client {
// Reuse media-only configuration and owned samples; transport uses client::Room.
using MediaInput = rtc::engine::MediaInput;
using MediaOutput = rtc::engine::MediaOutput;
using VideoSample = rtc::engine::VideoSample;
using AudioSample = rtc::engine::AudioSample;
using AudioOutputOptions = rtc::AudioOutputOptions;

struct MediaPublishSpec {
  std::string name;
  MediaKind kind = MediaKind::video;
  CodecFormat format;
  QueueLimits queue;
  TrackPublishOptions options;
  MediaInput input;
  MediaOutput preview;
  rtc::VideoCodecOptions video;
  rtc::AudioCodecOptions audio;
};

struct MediaSubscribeSpec {
  RemoteTrackPublication publication;
  MediaOutput output;
  rtc::VideoCodecOptions video;
  rtc::AudioCodecOptions audio = [] {
    rtc::AudioCodecOptions value;
    value.jitter_buffer = true;
    return value;
  }();
  bool synchronize = true;
  std::chrono::milliseconds presentation_delay{80};
};

struct MediaTrackSnapshot {
  TrackSnapshot transport;
  uint64_t frames = 0;
  Failure failure;
  std::vector<rtc::engine::MediaNodeSnapshot> nodes;
  std::vector<rtc::engine::MediaEdgeSnapshot> edges;
};

namespace detail {
class MediaPipeline;
struct MediaEngineTestAccess;
}  // namespace detail

// Owned handle; snapshots remain valid after unpublish/close. No graph/native types.
class MediaTrack {
 public:
  MediaTrackSnapshot snapshot() const;

 private:
  friend class MediaEngine;
  explicit MediaTrack(std::shared_ptr<detail::MediaPipeline> pipeline);
  std::shared_ptr<detail::MediaPipeline> pipeline_;
};

struct MediaPublication {
  LocalTrackPublication publication;
  std::shared_ptr<MediaTrack> track;
};

struct MediaSubscription {
  RemoteTrackPublication publication;
  std::shared_ptr<MediaTrack> track;
};

// Synchronous media owner over Room. Controls must run outside SDK/media callbacks;
// callbacks may query snapshots. close cancels pending Room operations and fences
// all capture/codec/output work. Destroy after concurrent public calls return.
// A closed engine is not reused. Observer lifetime/fencing follows Room exactly.
class MediaEngine {
 public:
  explicit MediaEngine(AudioOutputOptions = {});
  ~MediaEngine();
  MediaEngine(const MediaEngine&) = delete;
  MediaEngine& operator=(const MediaEngine&) = delete;
  void set_observer(RoomObserver*);
  bool connect(ConnectionOptions);
  void close();
  ConnectionState connection_state() const;
  RoomInfo info() const;
  LocalParticipant local_participant() const;
  std::vector<RemoteParticipant> remote_participants() const;
  Result<std::vector<PeerStatistics>> statistics();
  Result<MediaPublication> publish_media(MediaPublishSpec);
  Result<MediaSubscription> subscribe_media(MediaSubscribeSpec);
  void unpublish_track(TrackId);
  void unsubscribe_track(TrackSid);
  void set_muted(TrackId, bool);
  void set_receive_options(TrackSid, ReceiveOptions);
  void publish_data(DataPacket);
  void update(ParticipantUpdate);

 private:
  friend struct detail::MediaEngineTestAccess;
  MediaEngine(std::unique_ptr<Room>, AudioOutputOptions);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk::client
