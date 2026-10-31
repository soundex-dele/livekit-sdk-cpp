#pragma once
#include "livekit/result.hpp"
#include "backends/webrtc/transport_codecs.hpp"
#include "rtc_base/thread.h"
#ifdef _WIN32
#include "rtc_base/win32_socket_init.h"
#endif

namespace livekit::sdk::rtc::detail {

// Native transport resources only. Destroy all peers on signaling before this
// object. No graph, capture device, renderer or actual codec is constructed.
class TransportRuntime {
 public:
  // Optional native dependencies support headless network/transport testing.
  explicit TransportRuntime(bool enable_flexfec = true,
                            webrtc::PeerConnectionFactoryDependencies dependencies = {});
  ~TransportRuntime();
  TransportRuntime(const TransportRuntime&) = delete;
  TransportRuntime& operator=(const TransportRuntime&) = delete;

  Result<void> status() const {
    return failure_ ? Result<void>::failure(failure_) : Result<void>::success();
  }
  const bool flexfec_enabled;
  std::unique_ptr<webrtc::Thread> network, worker, signaling;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
  std::shared_ptr<CodecBoundaryCounters> codec_counters = std::make_shared<CodecBoundaryCounters>();

 private:
#ifdef _WIN32
  webrtc::WinsockInitializer winsock_;
#endif
  Failure failure_;
  bool ssl_ = false;
  void cleanup();
};
}  // namespace livekit::sdk::rtc::detail
