#pragma once
#include "backends/livekit/track_state.hpp"
#include "backends/livekit/media/webrtc_context.hpp"
#include "api/frame_transformer_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "livekit.pb.h"

namespace livekit::sdk::client::detail {
class PeerTransport : public webrtc::PeerConnectionObserver {
 public:
  PeerTransport(WebRtcContext&, const livekit::JoinResponse&, size_t max_events, MediaTransport);
  ~PeerTransport() override;
  Result<std::string> local_description(bool offer, const Operation&);
  Result<void> remote_description(const livekit::SessionDescription&, const Operation&);
  Result<void> add_ice(const livekit::TrickleRequest&);
  std::vector<std::string> take_ice();
  Result<void> create_data_channels();
  bool send_data(const std::string&, const std::string&);
  std::vector<std::string> take_data();
  bool bind_receiver(const TrackSid&, std::shared_ptr<TrackData>);
  void unbind_receiver(const TrackSid&);
  Result<void> keyframe(const TrackSid&);
  void close();
  Result<void> status() const;
  Result<PeerStatistics> statistics(const Operation&);

  bool connected() const {
    return state_ == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected;
  }

  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;

  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}

  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}

  void OnIceCandidate(const webrtc::IceCandidateInterface*) override;
  void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState) override;
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override;
  void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>) override;
  void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface>) override;

 private:
  class Receiver;
  struct Channel;

  struct Remote {
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver;
    webrtc::scoped_refptr<Receiver> sink;
  };

  WebRtcContext& context_;
  const size_t max_events_;
  mutable std::mutex mutex_;
  Failure failure_;
  std::deque<std::string> ice_, data_;
  size_t data_bytes_ = 0;
  // WebRTC signaling thread owns these registries.
  std::map<std::string, std::string> mids_;
  std::map<TrackSid, Remote> receivers_;
  std::vector<std::unique_ptr<Channel>> channels_;
  std::vector<livekit::TrickleRequest> pending_ice_;
  bool has_remote_ = false;
  std::atomic<webrtc::PeerConnectionInterface::PeerConnectionState> state_{
      webrtc::PeerConnectionInterface::PeerConnectionState::kNew};
};
}  // namespace livekit::sdk::client::detail
