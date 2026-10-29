#pragma once

#include "backends/rtc/room_transport.hpp"
#include "api/frame_transformer_interface.h"
#include <mutex>

namespace livekit::sdk::rtc::detail {

// A sender transformer with no PCM input. Its callback is registered by the
// native ChannelSend during negotiation, before any audio has been encoded.
// Each instance belongs to one binding; never reuse it across room epochs.
class EncodedAudioSender : public webrtc::FrameTransformerInterface {
 public:
  EncodedAudioSender(BindingToken binding, QueueLimits limits, std::string encoding_id = {});
  void configure(uint32_t ssrc, uint8_t payload_type);
  SendResult send(const EncodedFrame& frame);
  void set_muted(bool);
  void close();
  size_t outstanding_frames() const;
  size_t outstanding_bytes() const;

  void RegisterTransformedFrameCallback(
      webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback) override;
  void UnregisterTransformedFrameCallback() override;
  // PCM-derived frames are deliberately not forwarded. This backend accepts
  // externally encoded access units only.
  void Transform(std::unique_ptr<webrtc::TransformableFrameInterface>) override {}

 private:
  struct Budget;
  struct Reservation;
  BindingToken binding_;
  std::string encoding_id_;
  std::shared_ptr<Budget> budget_;
  mutable std::mutex mutex_;
  webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback_;
  uint32_t ssrc_ = 0;
  uint8_t payload_type_ = 0;
  bool configured_ = false, closed_ = false;
};

} // namespace livekit::sdk::rtc::detail
