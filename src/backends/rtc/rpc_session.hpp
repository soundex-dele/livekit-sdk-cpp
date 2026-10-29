#pragma once
#include "livekit/rpc.hpp"
#include "livekit.pb.h"
#include <future>
#include <memory>
#include <vector>

namespace livekit::sdk::detail {
class RpcSession {
 public:
  RpcSession();
  ~RpcSession();
  RpcSession(const RpcSession&) = delete;
  RpcSession& operator=(const RpcSession&) = delete;
  void connect();
  void disconnect();
  void participant_left(const std::string& identity);
  void register_method(const std::string& method, RpcHandler handler);
  void unregister_method(const std::string& method);
  std::future<std::string> call(const RpcCallOptions& options);
  void receive(const livekit::DataPacket& packet);
  bool has_outgoing() const;
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
