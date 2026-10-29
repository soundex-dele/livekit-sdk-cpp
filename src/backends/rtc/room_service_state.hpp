#pragma once
#include "livekit/rtc/room_services.hpp"
#include "backends/rtc/rpc_session.hpp"
#include "backends/rtc/stream_session.hpp"
#include "backends/rtc/data_track_session.hpp"
#include "backends/rtc/encryption.hpp"
#include <condition_variable>
#include <deque>

namespace livekit::sdk::rtc::detail {
struct RoomServiceState : std::enable_shared_from_this<RoomServiceState> {
  std::shared_ptr<RoomServices> handle();
  void configure(EncryptionOptions, std::chrono::milliseconds operation_timeout);
  void connect();
  void interrupt();
  void close();
  void participant(const livekit::ParticipantInfo&);
  void receive_signal(const livekit::SignalResponse&);
  void receive(const std::string& label, std::span<const uint8_t>);
  using Send = std::function<bool(const std::string&, std::span<const uint8_t>)>;
  void pump(const Send&, const std::function<void(const livekit::SignalRequest&)>&,
            const std::function<bool()>& reliable_ready = {});
  std::future<void> enqueue(livekit::DataPacket, DataSendOptions);
  std::optional<DataEvent> next_event(std::chrono::milliseconds, std::stop_token);

  livekit::sdk::detail::RpcSession rpc;
  std::shared_ptr<livekit::sdk::detail::StreamSession> streams =
      std::make_shared<livekit::sdk::detail::StreamSession>();
  std::shared_ptr<livekit::sdk::detail::DataTrackSession> tracks =
      std::make_shared<livekit::sdk::detail::DataTrackSession>();

 private:
  struct Pending {
    livekit::DataPacket packet;
    size_t bytes;
    std::shared_ptr<std::promise<void>> promise;
    std::chrono::steady_clock::time_point deadline;
    std::stop_token stop;
  };

  std::mutex mutex;
  std::condition_variable_any changed;
  bool active = false, closed = false;
  EncryptionOptions encryption;
  std::chrono::milliseconds timeout{10000};
  std::deque<Pending> outgoing;
  std::deque<DataEvent> incoming;
  size_t outgoing_bytes = 0, incoming_bytes = 0;
  // Worker-only packets already taken from RpcSession but awaiting native capacity.
  std::deque<livekit::DataPacket> rpc_pending;
  void emit(DataEvent);
  bool send_packet(const livekit::DataPacket&, const Send&);
};
}  // namespace livekit::sdk::rtc::detail
