#pragma once
#include "backends/rtc/transport_codecs.hpp"
#include "rtc_base/thread.h"
#ifdef _WIN32
#include "rtc_base/win32_socket_init.h"
#endif

namespace livekit::sdk::rtc::detail {

// Native transport resources only. Destroy all peers on signaling before this
// object. No graph, capture device, renderer or actual codec is constructed.
class TransportRuntime {
 public:
  TransportRuntime();
  ~TransportRuntime();
  TransportRuntime(const TransportRuntime&) = delete;
  TransportRuntime& operator=(const TransportRuntime&) = delete;
  std::unique_ptr<webrtc::Thread> network, worker, signaling;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
  std::shared_ptr<CodecBoundaryCounters> codec_counters = std::make_shared<CodecBoundaryCounters>();
 private:
#ifdef _WIN32
  webrtc::WinsockInitializer winsock_;
#endif
  bool ssl_ = false;
  void cleanup();
};
} // namespace livekit::sdk::rtc::detail
