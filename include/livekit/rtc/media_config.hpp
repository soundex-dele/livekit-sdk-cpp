#pragma once
#include "livekit/result.hpp"
#include <mutex>
#include "livekit/media_core/frame_view.hpp"
#include "livekit/rtc/audio_processing.hpp"
#include "livekit/rtc/media_types.hpp"
#include "livekit/video_source.hpp"
#include "livekit/microphone.hpp"
#include "livekit/encoded_media.hpp"
#include "livekit/rtc/audio_output_options.hpp"
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
  // Microphone inputs default to APM; application/file inputs opt in explicitly.
  std::optional<AudioProcessingOptions> audio_processing;
  std::shared_ptr<AudioRenderReference> echo_reference;
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
  std::optional<SteadyClock::time_point> presentation_time;
};

struct AudioSample {
  std::shared_ptr<const PcmAudioFrame> pcm;
  uint64_t timestamp = 0;
  Epoch epoch = 0;
  SteadyClock::time_point captured;
  bool discontinuity = false;
  std::optional<SteadyClock::time_point> presentation_time;
};

struct MediaOutput {
  // Runs on this output's bounded dispatcher. Slow callbacks drop only this
  // branch's backlog. Never wait for engine lifecycle operations from callbacks:
  // shutdown waits for the in-flight callback. Retained samples own their data.
  std::function<void(const VideoSample&)> video;
  std::function<void(const AudioSample&)> audio;
  std::function<void(const media::FrameView&)> frame;
  bool playout = false;
  std::filesystem::path recording;
  QueueLimits queue{4, 32 * 1024 * 1024, std::chrono::milliseconds(250)};
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
  bool synchronize = true;
  std::chrono::milliseconds presentation_delay{80};
  TrackDescriptor track;
  MediaOutput output;
  VideoCodecOptions video;
  AudioCodecOptions audio = [] {
    AudioCodecOptions options;
    options.jitter_buffer = true;
    return options;
  }();
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
  Result<void> check() const;

 private:
  friend class RtcMediaEngine;
  friend class RtcEngine;
  struct State;

  explicit MediaTrack(std::shared_ptr<State> state) : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

struct MediaPublication {
  TrackId id;
  std::shared_ptr<MediaTrack> track;
};

struct MediaSubscription {
  TrackId id;
  std::shared_ptr<MediaTrack> track;
};

}  // namespace livekit::sdk::rtc::engine
