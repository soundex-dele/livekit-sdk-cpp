#pragma once
#include "backends/rtc/room_transport.hpp"
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "pc/video_track_source.h"
#include "media/base/video_broadcaster.h"

namespace livekit::sdk::rtc::detail {
struct VideoSenderState;
struct CodecBoundaryCounters;

// One encoded track, one transport source. This is a native packet envelope;
// it owns no pixels and cannot perform capture, scaling or encoding.
class EncodedVideoSender : public webrtc::VideoTrackSource {
 public:
  EncodedVideoSender(BindingToken binding, TrackDescriptor descriptor);
  ~EncodedVideoSender() override;
  SendResult send(const EncodedFrame& frame);
  SenderFeedback feedback() const;
  void set_muted(bool);
  void set_demand(bool enabled);
  void close();

 protected:
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return &broadcaster_;
  }

 private:
  std::shared_ptr<VideoSenderState> state_;
  webrtc::VideoBroadcaster broadcaster_;
};

std::unique_ptr<webrtc::VideoEncoderFactory> create_transport_video_encoder_factory();
std::unique_ptr<webrtc::VideoDecoderFactory> create_transport_video_decoder_factory(
    std::shared_ptr<CodecBoundaryCounters> counters);
}  // namespace livekit::sdk::rtc::detail
