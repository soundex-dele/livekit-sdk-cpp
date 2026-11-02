#pragma once
#include "livekit/result.hpp"

#include "backends/livekit/track_config.hpp"
#include "support/wake_event.hpp"
#include "api/frame_transformer_interface.h"
#include <mutex>

namespace livekit::sdk::client::detail {

// A sender transformer with no PCM input. Its callback is registered by the
// native ChannelSend during negotiation, before any audio has been encoded.
// Each instance belongs to one binding; never reuse it across room epochs.
class EncodedAudioSender : public webrtc::FrameTransformerInterface {
 public:
  EncodedAudioSender(Epoch epoch, QueueLimits limits, std::string encoding_id = {},
                     std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup = {});
  Result<void> configure(uint32_t ssrc, uint8_t payload_type);
  WriteStatus send(const EncodedFrame& frame);
  void set_muted(bool);
  void close();

  Result<void> status() const {
    return failure_ ? Result<void>::failure(failure_) : Result<void>::success();
  }

  size_t outstanding_frames() const;
  size_t outstanding_bytes() const;

  void RegisterTransformedFrameCallback(
      webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback) override;
  void UnregisterTransformedFrameCallback() override;

  // PCM-derived frames are deliberately not forwarded. This backend accepts
  // externally encoded access units only.
  void Transform(std::unique_ptr<webrtc::TransformableFrameInterface>) override {}

 private:
  Failure failure_;
  struct Budget;
  struct Reservation;
  Epoch epoch_;
  std::string encoding_id_;
  std::shared_ptr<Budget> budget_;
  mutable std::mutex mutex_;
  webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback_;
  uint32_t ssrc_ = 0;
  uint8_t payload_type_ = 0;
  bool configured_ = false, closed_ = false;
};

}  // namespace livekit::sdk::client::detail
