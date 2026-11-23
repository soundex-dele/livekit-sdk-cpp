#pragma once
#include "livekit/media_core/frame_view.hpp"
#include "livekit/rtc/media_types.hpp"
#include "livekit/rtc/events.hpp"
#include "livekit/media.hpp"
#include "livekit/rtc/track_node.hpp"

namespace livekit::sdk::test {
struct CodecWorkerProbe;
}

namespace livekit::sdk::rtc::nodes {
// Immutable upright I420 pixels. Timestamp uses the track's 90 kHz clock;
// epoch is captured when input is admitted, never retagged after reconnect.

// Optional engine media nodes, linked through LiveKit::RTCMedia. Each owns a
// bounded worker and actual codec; neither creates a room or PeerConnection.
// Encoder: VideoFrameEvent -> EncodedFrameEvent, SenderFeedbackEvent input.
// Decoder: EncodedFrameEvent -> VideoFrameEvent, KeyframeRequestEvent output.
// Codec workers emit owned frames into bounded receiver mailboxes.
class VideoCodecNode final : public livekit::sdk::rtc::TrackNode {
 public:
  VideoCodecNode(std::string name, TrackDescriptor, bool encode, VideoCodecOptions = {});
  ~VideoCodecNode() override;
  VideoCodecSnapshot snapshot() const;
  Result<void> status() const;
  // Close admission and join worker before graph lifecycle stops downstream.
  // Idempotent; required as the owning TrackGraphSpec's quiesce hook.
  void quiesce();

 protected:
  void bind(graphflow::Binder& ports) override;
  Result<void> prepare() override;
  Result<void> activate_graph() override;
  void stop_graph() override;

 private:
  // Internal test observer: installed before binding, called on the encoder worker only.
  friend struct livekit::sdk::test::CodecWorkerProbe;
  std::function<void()> after_packet_;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk::rtc::nodes
