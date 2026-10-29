#pragma once
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/media_types.hpp"
#include "livekit/video_source.hpp"
#include "livekit/microphone.hpp"
#include "livekit/encoded_media.hpp"
#include "livekit/rtc/audio_output.hpp"
#include <filesystem>

namespace livekit::sdk::rtc::engine {
// Configuration and application-owned sources, never graph nodes. A microphone
// configuration creates an engine-owned device session. External sources are
// leased exclusively by one input reader and are not closed on unpublish.
struct MediaInput {
  bool generated = false, loop = false;
  std::string input, ffmpeg = "ffmpeg";
  CaptureKind capture = CaptureKind::input;
  int device_index = 0;
  std::string screen_id;
  uint32_t fps = 30;
  std::shared_ptr<I420VideoSource> video_source;
  std::shared_ptr<PcmAudioSource> audio_source;
  std::shared_ptr<EncodedMediaSource> encoded_source;
  std::optional<MicrophoneOptions> microphone;
};

struct VideoSample {
  std::shared_ptr<const VideoFrame> pixels;
  uint64_t timestamp = 0;
  Epoch epoch = 0;
  SteadyClock::time_point captured;
};

struct AudioSample {
  std::shared_ptr<const PcmAudioFrame> pcm;
  uint64_t timestamp = 0;
  Epoch epoch = 0;
  SteadyClock::time_point captured;
  bool discontinuity = false;
};

struct MediaOutput {
  // Runs on the input/codec worker. Must be bounded and nonblocking; never
  // reenter engine lifecycle from these callbacks. Retained samples own data.
  std::function<void(const VideoSample&)> video;
  std::function<void(const AudioSample&)> audio;
  bool playout = false;
  std::filesystem::path recording;
};

struct SharedInputSnapshot {
  size_t consumers = 0;
  bool running = false;
  uint64_t emitted = 0;
};

// One raw capture/reader feeding bounded queues for multiple publications,
// including publications in different rooms. First active graph starts capture;
// the last graph detaches and stops it. Keep this handle to share a device.
class SharedMediaInput {
 public:
  ~SharedMediaInput();
  SharedInputSnapshot snapshot() const;

 private:
  friend class RtcMediaEngine;
  struct Impl;

  explicit SharedMediaInput(std::shared_ptr<Impl> value) : impl_(std::move(value)) {}

  std::shared_ptr<Impl> impl_;
};

struct MediaPublishSpec {
  TrackDescriptor track;
  MediaInput input;
  MediaOutput preview;
  VideoCodecOptions video;
  AudioCodecOptions audio;
  std::shared_ptr<SharedMediaInput> shared_input;  // Replaces input; raw format must match track.
};

struct MediaSubscribeSpec {
  TrackDescriptor track;
  MediaOutput output;
  VideoCodecOptions video;
  AudioCodecOptions audio;
};

struct MediaNodeSnapshot {
  std::string name, kind;
  uint64_t inputs = 0, outputs = 0, dropped = 0, errors = 0;
};

struct MediaEdgeSnapshot {
  std::string from, to, data_type;
};

struct MediaTrackSnapshot {
  TrackSnapshot transport;
  std::optional<MediaInputSnapshot> input;
  std::optional<VideoCodecSnapshot> video;
  std::optional<AudioCodecSnapshot> audio;
  uint64_t frames = 0, bytes = 0;
  std::vector<MediaNodeSnapshot> nodes;
  std::vector<MediaEdgeSnapshot> edges;
};

class MediaTrack {
 public:
  MediaTrackSnapshot snapshot() const;
  void throw_if_failed() const;

 private:
  friend class RtcMediaEngine;
  struct State;

  explicit MediaTrack(std::shared_ptr<State> state) : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

struct MediaOperation {
  std::future<TrackId> ready;
  std::shared_ptr<MediaTrack> track;
};

// Optional layer above the encoded RtcEngine. All control/poll calls use the
// same owner thread. The engine owns graph construction and shutdown barriers.
class RtcMediaEngine final {
 public:
  explicit RtcMediaEngine(std::unique_ptr<RtcEngine>, AudioOutputOptions = {});
  ~RtcMediaEngine();
  MediaOperation publish_media(MediaPublishSpec);
  MediaOperation subscribe_media(MediaSubscribeSpec);
  MediaOperation publish_video(MediaPublishSpec);
  MediaOperation publish_audio(MediaPublishSpec);
  std::shared_ptr<SharedMediaInput> create_shared_input(TrackDescriptor raw_format, MediaInput);

  std::future<void> join(RoomOptions value) {
    return engine_->join(std::move(value));
  }

  std::future<void> unpublish(const TrackId& id);
  std::future<void> unsubscribe(const TrackId& id);

  std::future<void> set_muted(const TrackId& id, bool value, ControlOptions options = {}) {
    return engine_->set_muted(id, value, options);
  }

  std::future<void> set_receive_demand(const TrackId& id, ReceiveDemand value,
                                       ControlOptions options = {}) {
    return engine_->set_receive_demand(id, value, options);
  }

  void poll(size_t frames = 4);

  auto next_failure() {
    return engine_->next_failure();
  }

  std::future<void> close();

  RoomBackend& room() {
    return engine_->room();
  }

  TrackSnapshot publication(const TrackId& id) const {
    return engine_->publication(id);
  }

  SenderFeedback feedback(const TrackId& id) const {
    return engine_->feedback(id);
  }

  // Advanced custom graphs remain available explicitly on the encoded engine.
  RtcEngine& encoded_engine() {
    return *engine_;
  }

 private:
  MediaOperation operation(std::future<TrackId>, std::shared_ptr<MediaTrack::State>);
  void finish_pending();
  std::shared_ptr<AudioOutputSession> output_;
  std::unique_ptr<RtcEngine> engine_;
  std::vector<std::function<bool()>> pending_;
};

std::unique_ptr<RtcMediaEngine> create_livekit_media_engine(AudioOutputOptions = {});
}  // namespace livekit::sdk::rtc::engine
