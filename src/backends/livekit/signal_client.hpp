#pragma once
#include "backends/livekit/rtc_session.hpp"
#include "livekit.pb.h"
#include <ixwebsocket/IXWebSocket.h>
#include <deque>
#include <mutex>

namespace livekit::sdk::client::detail {
class SignalClient {
 public:
  ~SignalClient();
  Result<void> connect(const ConnectionOptions&);
  Result<void> send(const livekit::SignalRequest&);
  std::optional<livekit::SignalResponse> next();
  Result<void> status() const;
  void close();

 private:
  ix::WebSocket socket_;
  mutable std::mutex mutex_;
  std::deque<livekit::SignalResponse> messages_;
  size_t bytes_ = 0, max_events_ = 4096;
  Failure failure_;
};
}  // namespace livekit::sdk::client::detail
