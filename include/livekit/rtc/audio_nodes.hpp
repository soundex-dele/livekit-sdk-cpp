#pragma once
#include "livekit/media_core/frame_view.hpp"
#include "livekit/rtc/media_types.hpp"
#include "livekit/rtc/events.hpp"
#include "livekit/rtc/track_node.hpp"

namespace livekit::sdk::test {
struct CodecWorkerProbe;
}

namespace livekit::sdk::rtc::nodes {

// Optional engine codec, never a Room/PeerConnection owner. Encoder input is
// exactly 10 ms PCM16 or float32 at 8..96 kHz (rate divisible by 100), mono or
// stereo matching the descriptor. Resampling and actual Opus encoding run on
// its bounded worker. Decoder output is owned PCM16, 48 kHz: 10 ms in NetEq
// mode, or 2.5..120 ms in bare decoding mode.
// Connect SenderFeedbackEvent to the encoder. Codec workers emit into bounded receiver mailboxes. NetEq handles receive reordering;
// device playout remains a separate node.
class AudioCodecNode final : public livekit::sdk::rtc::TrackNode {
 public:
  AudioCodecNode(std::string name, TrackDescriptor, bool encode, AudioCodecOptions = {});
  ~AudioCodecNode() override;
  AudioCodecSnapshot snapshot() const;
  Result<void> status() const;
  void quiesce();  // Close admission and join worker before stopping downstream.
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
