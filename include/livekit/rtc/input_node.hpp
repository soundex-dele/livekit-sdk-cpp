#pragma once
#include "livekit/rtc/media_types.hpp"
#include "livekit/rtc/audio_nodes.hpp"
#include "livekit/rtc/video_nodes.hpp"
#include "livekit/audio_source.hpp"
#include "livekit/microphone.hpp"
#include "livekit/video_source.hpp"
#include "livekit/encoded_media.hpp"

namespace livekit::sdk::rtc::nodes {
struct MediaInputOptions {
  // Exactly one input: generated, file/URL, device, or application source.
  bool generated = false, loop = false;
  std::string input, ffmpeg = "ffmpeg";
  CaptureKind capture = CaptureKind::input;
  int device_index = 0;
  std::string screen_id;
  uint32_t fps = 30;
  std::shared_ptr<I420VideoSource> video_source;
  std::shared_ptr<PcmAudioSource> audio_source;
  std::shared_ptr<MicrophoneSession> microphone;
  std::shared_ptr<EncodedMediaSource> encoded_source;
};
// Engine-level source. No Room/PeerConnection, RTC encoder or renderer is owned
// here. File/URL input uses an external FFmpeg decoder above the RTC backend.
// Raw sources emit VideoFrameEvent/AudioFrameEvent; encoded sources emit
// EncodedFrameEvent directly. Connect SenderFeedbackEvent from the RTC sink.
// Delivery is synchronous on the producer thread; consumers must not block.
class MediaInputNode final : public graphflow::core::GraphNode {
 public:
  MediaInputNode(std::string name, TrackDescriptor, MediaInputOptions);
  ~MediaInputNode() override;
  void activate(); // TrackGraphSpec::activate, after downstream graph start.
  void quiesce(); // TrackGraphSpec::quiesce, before downstream graph stop.
  MediaInputSnapshot snapshot() const;
  void throw_if_failed() const;
 protected:
  bool doInit() override;
  bool doStart() override;
  void doStop() override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace livekit::sdk::rtc::nodes
