#pragma once
#include "backends/rtc/livekit/signal_socket.hpp"
#include "backends/rtc/events.hpp"
#include "livekit.pb.h"
#include "backends/rtc/livekit/signal_recovery_state.hpp"
#include "livekit/rtc/signaling_options.hpp"
#include <functional>

namespace lk {
using livekit::sdk::rtc::SignalingOptions;

// Session-thread owner. WebRTC producers may only enqueue ICE; transport loss
// never turns a WebRTC observer callback into an exception or media failure.
class SignalSession {
 public:
  using SyncBuilder = std::function<void(livekit::SyncState&)>;
  using ReconnectHandler = std::function<void(const livekit::ReconnectResponse&)>;
  using MovedHandler = std::function<void(const livekit::RoomMovedResponse&)>;
  SignalSession(const SignalingOptions& options, std::stop_token stop, EventEmitter events,
                livekit::sdk::detail::SignalRecoveryState* recovery = nullptr,
                std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup = {});
  void connect();

  void initial_offer(const livekit::SessionDescription& offer,
                     const std::vector<livekit::AddTrackRequest>& tracks = {}) {
    initial_offer_ = offer;
    initial_tracks_ = tracks;
  }

  // Only before accepting JoinResponse. Closes the previous socket first and
  // never retries permission/protocol errors or revisits an attempted endpoint.
  bool failover(const livekit::sdk::SDKError& error);
  void redirect(const std::string& url);
  void update_regions(const livekit::RegionSettings& regions);

  void configure_client(const livekit::ClientConfiguration& config) {
    resume_allowed_ = config.resume_connection() != livekit::DISABLED;
  }

  std::chrono::steady_clock::time_point connect_deadline() const noexcept {
    return attempt_deadline_;
  }

  std::stop_token stop_token() const noexcept {
    return stop_;
  }

  std::chrono::steady_clock::time_point operation_deadline() const noexcept {
    auto deadline = std::min(recovery_.budget.deadline(), recovery_.session_deadline);
    if (!recovery_.ever_connected || recovery_.rejoining)
      deadline = std::min(deadline, connect_deadline_);
    return std::min(deadline, std::chrono::steady_clock::now() +
                                  std::chrono::seconds(options_.connect_timeout));
  }

  void bind_participant(std::string sid);
  void update_token(std::string token);
  void send(const livekit::SignalRequest& request);
  void enqueue_ice(const livekit::SignalRequest& request) noexcept;
  void flush_ice();
  std::optional<std::string> receive(int timeout_ms);

  std::optional<std::string> receive_until(std::chrono::steady_clock::time_point deadline) {
    return socket_.receive_until(deadline, stop_);
  }

  std::optional<std::string> take_message() {
    return socket_.take_message();
  }
  bool recover(const SyncBuilder& sync, const ReconnectHandler& configure,
               const MovedHandler& moved = {});

  size_t recovery_attempt() const noexcept {
    return recovery_attempt_;
  }

  void close();

 private:
  void connect_socket(const std::string& sid, std::chrono::steady_clock::time_point deadline);
  bool select_initial_fallback(const livekit::sdk::SDKError& error);
  void open_initial_socket();
  livekit::SessionDescription initial_offer_;
  std::vector<livekit::AddTrackRequest> initial_tracks_;
  SignalingOptions options_;
  std::stop_token stop_;
  EventEmitter events_;
  SignalSocket socket_;
  livekit::sdk::detail::SignalRecoveryState local_recovery_;
  livekit::sdk::detail::SignalRecoveryState& recovery_;
  std::string sid_;
  std::mutex ice_mutex_;
  std::deque<std::string> ice_;
  bool ice_overflow_ = false;
  bool resume_allowed_ = true;
  size_t recovery_attempt_ = 0;
  size_t redirects_ = 0;
  std::set<std::string> redirect_history_;
  std::chrono::steady_clock::time_point connect_deadline_ =
      std::chrono::steady_clock::time_point::max();
  std::chrono::steady_clock::time_point attempt_deadline_ =
      std::chrono::steady_clock::time_point::max();
  bool initial_failover_ = false;
};
}  // namespace lk
