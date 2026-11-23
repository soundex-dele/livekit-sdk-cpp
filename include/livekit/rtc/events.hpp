#pragma once
#include "livekit/rtc/types.hpp"
#include "livekit/media_core/frame_view.hpp"
#include "livekit/rtc/media_types.hpp"
#include "livekit/media.hpp"
#include <atomic>

namespace livekit::sdk::rtc::nodes {
struct EncodedFrameEvent {
  explicit EncodedFrameEvent(EncodedFrame value) : frame(std::move(value)) {}

  EncodedFrame frame;
};

struct SenderFeedbackEvent {
  explicit SenderFeedbackEvent(SenderFeedback value) : feedback(std::move(value)) {}

  SenderFeedback feedback;
};

struct KeyframeRequestEvent {};

struct VideoFrameEvent : media::FrameTiming {
  VideoFrameEvent() {
    clock_rate = 90000;
  }

  std::shared_ptr<const livekit::sdk::VideoFrame> pixels;
};

struct AudioFrameEvent : media::FrameTiming {
  AudioFrameEvent() {
    clock_rate = 48000;
  }

  std::shared_ptr<const PcmAudioFrame> pcm;
  // A gap/epoch/format boundary. Bare decoding preserves gaps; NetEq mode
  // conceals loss and produces a paced output clock.
  bool discontinuity = false;
};
}  // namespace livekit::sdk::rtc::nodes
