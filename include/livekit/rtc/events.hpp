#pragma once
#include "livekit/rtc/types.hpp"
#include "core/event.h"
#include <atomic>

namespace livekit::sdk::rtc::nodes {
struct EncodedFrameEvent : graphflow::core::EventT<EncodedFrameEvent> {
  explicit EncodedFrameEvent(EncodedFrame value) : frame(std::move(value)) {}

  EncodedFrame frame;
  // Optional receipt for synchronous engine ingress. Independent of other
  // producers concurrently publishing to the same RTC sink.
  std::shared_ptr<std::atomic<WriteStatus>> receipt;
};

struct SenderFeedbackEvent : graphflow::core::EventT<SenderFeedbackEvent> {
  explicit SenderFeedbackEvent(SenderFeedback value) : feedback(std::move(value)) {}

  SenderFeedback feedback;
};

struct KeyframeRequestEvent : graphflow::core::EventT<KeyframeRequestEvent> {};
}  // namespace livekit::sdk::rtc::nodes
