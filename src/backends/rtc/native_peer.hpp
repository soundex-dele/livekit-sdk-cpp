#pragma once
#include "backends/rtc/encoded_receiver.hpp"
#include "backends/rtc/transport_runtime.hpp"
#include "api/rtp_transceiver_interface.h"
#include <deque>
#include <future>
#include <map>

namespace livekit::sdk::rtc::detail {

struct NativeDescription {
  webrtc::SdpType type;
  std::string sdp;
};

struct NativeIceCandidate {
  std::string mid;
  int mline_index = 0;
  std::string sdp;
};

struct NativeRemoteTrack {
  std::string sid;
  std::string mid;
  MediaKind kind = MediaKind::audio;
  webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver;
  webrtc::scoped_refptr<EncodedReceiver> output;
};

struct NativeTrackEvent {
  bool added;
  std::string sid;
};

struct NativeDataMessage {
  std::string label;
  std::vector<uint8_t> bytes;
};

// Peer/session transport with asynchronous SDP completion. The room controller
// polls futures and queues instead of waiting on a native callback. All native
// callbacks end at owned queues; no graph/application callback is invoked.
class NativePeer : public webrtc::PeerConnectionObserver {
 public:
  NativePeer(TransportRuntime& runtime,
             webrtc::PeerConnectionInterface::RTCConfiguration configuration,
             QueueLimits receive_limits = {}, size_t max_events = 4096,
             std::shared_ptr<QueueBudget> budget = {});
  ~NativePeer() override;
  NativePeer(const NativePeer&) = delete;
  NativePeer& operator=(const NativePeer&) = delete;

  std::future<NativeDescription> create_local(bool offer, bool ice_restart = false);
  std::future<TransportStatistics> statistics();
  std::future<void> set_remote(NativeDescription description,
                               std::map<std::string, std::string> mid_to_track = {});
  // Call when offer/answer negotiation is stable to bind reused/preallocated receivers.
  void refresh_receivers();
  void add_ice(const NativeIceCandidate& candidate);
  std::vector<NativeIceCandidate> take_ice();
  std::vector<NativeTrackEvent> take_track_events();
  std::map<std::string, NativeRemoteTrack> remote_tracks() const;
  void request_keyframe(const std::string& sid);
  void reset_receiver(const std::string& sid);
  using ReceiverTransformFactory =
      std::function<EncodedReceiver::PayloadTransform(const std::string&, MediaKind)>;
  void set_receiver_transform(ReceiverTransformFactory);
  void create_data_channel(const std::string& label, bool reliable);
  bool data_ready(const std::string& label, size_t bytes = 16384);
  bool send_data(const std::string& label, std::span<const uint8_t> bytes);
  std::vector<NativeDataMessage> take_data(size_t max_messages = 64);
  void close();

  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
  std::atomic<bool> connected{false}, gathered{false}, failed{false};

  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}

  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override;
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override;
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
  void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override;
  void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override;
  void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;

 private:
  TransportRuntime& runtime_;
  QueueLimits receive_limits_;
  std::shared_ptr<QueueBudget> budget_;
  size_t max_events_;
  mutable std::mutex mutex_;
  std::deque<NativeIceCandidate> ice_;
  std::deque<NativeTrackEvent> tracks_;
  std::map<std::string, NativeRemoteTrack> remote_tracks_;
  // Signaling thread only.
  std::map<std::string, std::string> mids_;
  ReceiverTransformFactory receiver_transform_;
  struct Channel;
  std::vector<std::unique_ptr<Channel>> channels_;
  std::deque<NativeDataMessage> data_;
  size_t data_bytes_ = 0;
  static constexpr size_t max_data_bytes = 4 * 1024 * 1024;
  bool closed_ = false;
  void track_event(bool added, const std::string& sid);
};
}  // namespace livekit::sdk::rtc::detail
