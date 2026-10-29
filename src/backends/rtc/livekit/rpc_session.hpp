#pragma once
#include "livekit/rpc.hpp"
#include "livekit.pb.h"
#include <future>
#include <memory>
#include <vector>
#include <optional>
#include <chrono>
#include <cstdint>

namespace livekit::sdk::detail {
class WakeEvent;

struct RpcSessionSnapshot {
  size_t outgoing_requests = 0, incoming_requests = 0, queued_packets = 0;
  size_t scheduled_deadlines = 0, pending_cancellations = 0;
  uint64_t requests_serviced = 0;
  std::optional<std::chrono::steady_clock::time_point> next_deadline;
};

class RpcSession {
 public:
  RpcSession();
  ~RpcSession();
  RpcSession(const RpcSession&) = delete;
  RpcSession& operator=(const RpcSession&) = delete;
  // Native send readiness only; deadline/cancellation completion remains autonomous.
  void attach_wakeup(std::shared_ptr<WakeEvent>);
  void connect();
  void disconnect();
  void participant_left(const std::string& identity);
  void register_method(const std::string& method, RpcHandler handler);
  void unregister_method(const std::string& method);
  std::future<std::string> call(const RpcCallOptions& options);
  void receive(const livekit::DataPacket& packet);
  bool has_outgoing() const;
  RpcSessionSnapshot snapshot() const;
  bool sendable(const livekit::DataPacket&) const;
  std::vector<livekit::DataPacket> take_packets(size_t max_packets = 8);
  void send_failed(const livekit::DataPacket& packet);

 private:
  struct State;
  std::shared_ptr<State> state_;
  struct Threads;
  std::unique_ptr<Threads> threads_;
};
}  // namespace livekit::sdk::detail
