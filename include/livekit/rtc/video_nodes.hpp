#pragma once
#include "livekit/media_core/frame_view.hpp"
#include "livekit/rtc/media_types.hpp"
#include "livekit/rtc/events.hpp"
#include "livekit/media.hpp"
#include "core/graph_node.h"

namespace livekit::sdk::rtc::nodes {
// Immutable upright I420 pixels. Timestamp uses the track's 90 kHz clock;
// epoch is captured when input is admitted, never retagged after reconnect.
struct VideoFrameEvent : graphflow::core::EventT<VideoFrameEvent>, media::FrameTiming {
  VideoFrameEvent() {
    clock_rate = 90000;
  }
  std::shared_ptr<const livekit::sdk::VideoFrame> pixels;
};

// Optional engine media nodes, linked through LiveKit::RTCMedia. Each owns a
// bounded worker and actual codec; neither creates a room or PeerConnection.
// Encoder: VideoFrameEvent -> EncodedFrameEvent, SenderFeedbackEvent input.
// Decoder: EncodedFrameEvent -> VideoFrameEvent, KeyframeRequestEvent output.
// Downstream callbacks execute on the codec worker and must be nonblocking.
class VideoCodecNode final : public graphflow::core::GraphNode {
 public:
  VideoCodecNode(std::string name, TrackDescriptor, bool encode, VideoCodecOptions = {});
  ~VideoCodecNode() override;
  VideoCodecSnapshot snapshot() const;
  void throw_if_failed() const;
  // Close admission and join worker before graph lifecycle stops downstream.
  // Idempotent; required as the owning TrackGraphSpec's quiesce hook.
  void quiesce();

 protected:
  bool doInit() override;
  bool doStart() override;
  void doStop() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk::rtc::nodes
