#pragma once
#include "support/net.hpp"
#include "engines/rtc/events.hpp"
#include "livekit.pb.h"
#include "engines/rtc/recovery_state.hpp"
#include <functional>

namespace lk {
using livekit::sdk::RoomOptions;
// Session-thread owner. WebRTC producers may only enqueue ICE; transport loss
// never turns a WebRTC observer callback into an exception or media failure.
class SignalSession {
 public:
  using SyncBuilder = std::function<void(livekit::SyncState&)>;
  using ReconnectHandler = std::function<void(const livekit::ReconnectResponse&)>;
  using MovedHandler = std::function<void(const livekit::RoomMovedResponse&)>;
  SignalSession(const RoomOptions& options, std::stop_token stop, EventEmitter events,
                livekit::sdk::detail::RecoveryState* recovery = nullptr);
  void connect();
  void redirect(const std::string& url);
  void update_regions(const livekit::RegionSettings& regions);
  void configure_client(const livekit::ClientConfiguration& config) { resume_allowed_ = config.resume_connection() != livekit::DISABLED; }
  std::chrono::steady_clock::time_point connect_deadline() const noexcept { return connect_deadline_; }
  std::stop_token stop_token() const noexcept { return stop_; }
  std::chrono::steady_clock::time_point operation_deadline() const noexcept {
    auto deadline = std::min(recovery_.budget.deadline(), recovery_.session_deadline);
    if (!recovery_.ever_connected || recovery_.rejoining) deadline = std::min(deadline, connect_deadline_);
    return std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(options_.connect_timeout));
  }
  void bind_participant(std::string sid);
  void update_token(std::string token);
  void send(const livekit::SignalRequest& request);
  void enqueue_ice(const livekit::SignalRequest& request) noexcept;
  void flush_ice();
  std::optional<std::string> receive(int timeout_ms);
  bool recover(const SyncBuilder& sync, const ReconnectHandler& configure, const MovedHandler& moved = {});
  size_t recovery_attempt() const noexcept { return recovery_attempt_; }
  void close();
 private:
  void connect_socket(const std::string& sid, std::chrono::steady_clock::time_point deadline);
  RoomOptions options_;
  std::stop_token stop_;
  EventEmitter events_;
  SignalSocket socket_;
  livekit::sdk::detail::RecoveryState local_recovery_;
  livekit::sdk::detail::RecoveryState& recovery_;
  std::string sid_;
  std::mutex ice_mutex_;
  std::deque<std::string> ice_;
  bool ice_overflow_ = false;
  bool resume_allowed_ = true;
  size_t recovery_attempt_ = 0;
  size_t redirects_ = 0;
  std::set<std::string> redirect_history_;
  std::chrono::steady_clock::time_point connect_deadline_ = std::chrono::steady_clock::time_point::max();
};
}
