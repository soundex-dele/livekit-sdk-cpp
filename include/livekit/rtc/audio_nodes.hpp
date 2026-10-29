#pragma once
#include "livekit/rtc/media_types.hpp"
#include "livekit/rtc/events.hpp"
#include "core/graph_node.h"

namespace livekit::sdk::rtc::nodes {
struct AudioFrameEvent : graphflow::core::EventT<AudioFrameEvent> {
  std::shared_ptr<const PcmAudioFrame> pcm;
  uint64_t timestamp = 0; // 48 kHz track clock, independent of input sample rate.
  Epoch epoch = 0;
  SteadyClock::time_point captured = SteadyClock::now();
  // A gap/epoch/format boundary. Consumers must preserve the timestamp gap;
  // this codec node does not silently fill, compress or pace the timeline.
  bool discontinuity = false;
};
// Optional engine codec, never a Room/PeerConnection owner. Encoder input is
// exactly 10 ms PCM16 or float32 at 8..96 kHz (rate divisible by 100), mono or
// stereo matching the descriptor. Resampling and actual Opus encoding run on
// its bounded worker. Decoder output is owned PCM16, 48 kHz, 2.5..120 ms.
// Connect SenderFeedbackEvent to the encoder. Downstream callbacks run on the
// codec worker and must be nonblocking; playout/reordering belong above it.
class AudioCodecNode final : public graphflow::core::GraphNode {
 public:
  AudioCodecNode(std::string name, TrackDescriptor, bool encode, AudioCodecOptions = {});
  ~AudioCodecNode() override;
  AudioCodecSnapshot snapshot() const;
  void throw_if_failed() const;
  void quiesce(); // Close admission and join worker before stopping downstream.
 protected:
  bool doInit() override;
  bool doStart() override;
  void doStop() override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace livekit::sdk::rtc::nodes
