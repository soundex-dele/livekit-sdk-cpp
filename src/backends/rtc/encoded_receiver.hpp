#pragma once
#include "livekit/rtc/types.hpp"
#include "api/frame_transformer_interface.h"
#include <deque>
#include <functional>
#include <mutex>

namespace livekit::sdk::rtc::detail {

struct ReceivedAccessUnit {
  EncodedBuffer buffer;
  uint32_t timestamp = 0, ssrc = 0;
  uint32_t width = 0, height = 0;
  bool keyframe = false;
  std::string mime_type;
};

// Terminal receive boundary: owns complete depacketized access units and never
// calls the native decoder callback. poll() transfers ownership to RoomBackend.
// Native callback work is bounded even while the controller is busy signaling.
class EncodedReceiver : public webrtc::FrameTransformerInterface {
 public:
  using PayloadTransform = std::function<std::optional<std::vector<uint8_t>>(std::span<const uint8_t>)>;
  EncodedReceiver(MediaKind kind, QueueLimits limits, std::shared_ptr<QueueBudget> budget = {}, PayloadTransform transform = {});
  void Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override;
  std::vector<ReceivedAccessUnit> poll();
  // Installed before exposing a new native receiver as a subscribed endpoint.
  void configure_queue(QueueLimits, std::shared_ptr<QueueBudget>);
  bool take_keyframe_request();
  void close();
  uint64_t dropped() const;
 private:
  PayloadTransform transform_;
  MediaKind kind_;
  QueueLimits limits_;
  std::shared_ptr<QueueBudget> budget_;
  mutable std::mutex mutex_;
  struct Entry { ReceivedAccessUnit frame; SteadyClock::time_point arrival; };
  std::deque<Entry> queue_;
  size_t bytes_ = 0;
  uint64_t dropped_ = 0;
  bool closed_ = false, needs_keyframe_ = false, request_keyframe_ = false;
  void discard_chain();
  void receive(std::unique_ptr<webrtc::TransformableFrameInterface> frame);
};
} // namespace livekit::sdk::rtc::detail
