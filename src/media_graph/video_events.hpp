#pragma once
#include "media_graph/rtc_graph.hpp"
#include "api/video/video_frame.h"
#include <functional>

namespace livekit::sdk::detail {
struct RawVideoFrame : graphflow::core::EventT<RawVideoFrame> {
  explicit RawVideoFrame(const webrtc::VideoFrame& value) : frame(value) {}
  webrtc::VideoFrame frame;
};
struct EncodedVideoFrame : graphflow::core::EventT<EncodedVideoFrame> {
  explicit EncodedVideoFrame(const webrtc::VideoFrame& value) : frame(value) {}
  webrtc::VideoFrame frame; // Refcounted native packet envelope, never raw pixels.
};
// Synchronous edge only. Recording copies PCM into its bounded queue before
// returning; the capture/WebRTC buffer remains valid throughout dispatch.
struct PcmFrame : graphflow::core::EventT<PcmFrame> {
  PcmFrame(const void* data, int bits, int rate, size_t channels, size_t frames)
      : data(data), bits(bits), rate(rate), channels(channels), frames(frames) {}
  const void* data;
  int bits, rate;
  size_t channels, frames;
};

// Adapter at an external API boundary. Internal processing nodes subscribe to
// typed edges; no downstream object is required by the producer.
template<class Event> class MediaEventNode final : public MediaNode {
 public:
  using Handler = std::function<void(Event&)>;
  MediaEventNode(std::string kind, NodeExecution execution, Handler handler = {})
      : MediaNode(std::move(kind), execution), handler_(std::move(handler)) {}
  void emit(const Event& event) {
    if (!running_) { ++dropped; return; }
    ++inputs; publish(event); ++outputs;
  }
 protected:
  bool doInit() override {
    subscribe<Event>([this](Event& event) {
      if (!running_) { ++dropped; return; }
      ++inputs;
      try { if (handler_) handler_(event); else publish(event); ++outputs; }
      catch (...) { ++errors; throw; }
    });
    return true;
  }
  bool doStart() override { running_ = true; return true; }
  void doStop() override { running_ = false; }
 private:
  Handler handler_;
  std::atomic<bool> running_{false};
};
}
