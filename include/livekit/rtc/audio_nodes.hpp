#pragma once
#include "livekit/media_core/frame_view.hpp"
#include "livekit/rtc/media_types.hpp"
#include "livekit/rtc/events.hpp"
#include "core/graph_node.h"

namespace livekit::sdk::rtc::nodes {
struct AudioFrameEvent : graphflow::core::EventT<AudioFrameEvent>, media::FrameTiming {
  AudioFrameEvent() {
    clock_rate = 48000;
  }
  std::shared_ptr<const PcmAudioFrame> pcm;
  // A gap/epoch/format boundary. Bare decoding preserves gaps; NetEq mode
  // conceals loss and produces a paced output clock.
  bool discontinuity = false;
};

// Optional engine codec, never a Room/PeerConnection owner. Encoder input is
// exactly 10 ms PCM16 or float32 at 8..96 kHz (rate divisible by 100), mono or
// stereo matching the descriptor. Resampling and actual Opus encoding run on
// its bounded worker. Decoder output is owned PCM16, 48 kHz: 10 ms in NetEq
// mode, or 2.5..120 ms in bare decoding mode.
// Connect SenderFeedbackEvent to the encoder. Downstream callbacks run on the
// codec worker and must be nonblocking. NetEq handles receive reordering;
// device playout remains a separate node.
class AudioCodecNode final : public graphflow::core::GraphNode {
 public:
  AudioCodecNode(std::string name, TrackDescriptor, bool encode, AudioCodecOptions = {});
  ~AudioCodecNode() override;
  AudioCodecSnapshot snapshot() const;
  void throw_if_failed() const;
  void quiesce();  // Close admission and join worker before stopping downstream.
 protected:
  bool doInit() override;
  bool doStart() override;
  void doStop() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk::rtc::nodes
