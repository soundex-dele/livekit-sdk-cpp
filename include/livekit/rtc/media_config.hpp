#pragma once
#include "livekit/rtc/media_types.hpp"
#include "livekit/video_source.hpp"
#include "livekit/microphone.hpp"
#include "livekit/encoded_media.hpp"
#include "livekit/rtc/audio_output.hpp"
#include <filesystem>
#include <functional>
#include <future>

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

}  // namespace livekit::sdk::rtc::engine
