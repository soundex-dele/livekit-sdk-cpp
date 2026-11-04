#pragma once
#include "livekit/result.hpp"
#include "backends/livekit/media/codec_envelopes.hpp"
#include "backends/livekit/media/video_receive_bridge.hpp"
#include "rtc_base/thread.h"
#ifdef _WIN32
#include "rtc_base/win32_socket_init.h"
#endif

namespace livekit::sdk::client::detail {

// Native transport resources only. Destroy all peers on signaling before this
// object. No graph, capture device, renderer or actual codec is constructed.
class WebRtcContext {
 public:
  // Optional native dependencies support headless network/transport testing.
  explicit WebRtcContext(bool enable_flexfec = true,
                         webrtc::PeerConnectionFactoryDependencies dependencies = {});
  ~WebRtcContext();
  WebRtcContext(const WebRtcContext&) = delete;
  WebRtcContext& operator=(const WebRtcContext&) = delete;

  Result<void> status() const {
    return failure_ ? Result<void>::failure(failure_) : Result<void>::success();
  }

  const bool flexfec_enabled;
  std::unique_ptr<webrtc::Thread> network, worker, signaling;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
  std::shared_ptr<CodecBoundaryCounters> codec_counters = std::make_shared<CodecBoundaryCounters>();
  std::shared_ptr<VideoReceiveRoutes> video_receivers = std::make_shared<VideoReceiveRoutes>();

 private:
#ifdef _WIN32
  webrtc::WinsockInitializer winsock_;
#endif
  Failure failure_;
  bool ssl_ = false;
  void cleanup();
};
}  // namespace livekit::sdk::client::detail
