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
  // Return a failure to fail this branch. Callbacks must not raise exceptions.
  std::function<Result<void>(const VideoSample&)> video;
  std::function<Result<void>(const AudioSample&)> audio;
  std::function<Result<void>(const media::FrameView&)> frame;
  bool playout = false;
  std::filesystem::path recording;
  QueueLimits queue{4, 32 * 1024 * 1024, std::chrono::milliseconds(250)};
};

struct MediaNodeSnapshot {
  std::string name, kind;
  uint64_t inputs = 0, outputs = 0, dropped = 0, errors = 0;
};

struct MediaEdgeSnapshot {
  std::string from, to, data_type;
};

}  // namespace livekit::sdk::rtc::engine
