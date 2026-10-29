#pragma once
#include "livekit/rtc/room_services.hpp"
#include "backends/rtc/livekit/rpc_session.hpp"
#include "backends/rtc/livekit/stream_session.hpp"
#include "backends/rtc/livekit/data_track_session.hpp"
#include "backends/rtc/livekit/packet_encryption.hpp"
#include <condition_variable>
#include <deque>
#include <thread>
#include "support/ready_set.hpp"
#include "support/deadline_index.hpp"
#include <map>

namespace livekit::sdk::rtc::detail {
struct RoomServiceState : std::enable_shared_from_this<RoomServiceState> {
  std::shared_ptr<RoomServices> handle();
  void configure(EncryptionOptions, std::chrono::milliseconds operation_timeout);
  // Install before connecting; native send/deadline/cancellation work wakes this owner.
  void attach_wakeup(std::shared_ptr<livekit::sdk::detail::WakeEvent>);
  std::optional<std::chrono::steady_clock::time_point> next_data_deadline() const;
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
  using Dispatch = std::function<void(std::function<void()>)>;
  void set_dispatcher(Dispatch, std::function<void(std::exception_ptr)>);
  void set_event_handler(std::function<void(const DataEvent&)>);
  RoomServiceSnapshot snapshot() const;

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
    std::shared_ptr<std::stop_callback<std::function<void()>>> cancellation;
  };

  struct Handler {
    std::function<void(const DataEvent&)> call;
    bool running = false;
    std::thread::id thread;
  };

  mutable std::mutex mutex;
  std::shared_ptr<Handler> handler, in_flight;
  Dispatch dispatch;
  std::function<void(std::exception_ptr)> report_failure;
  bool notification_scheduled = false;
  uint64_t dropped_events = 0;
  std::shared_ptr<const FailureInfo> callback_failure;
  void schedule_events_locked();
  void deliver_events();
  std::condition_variable_any changed;
  bool active = false, closed = false;
  EncryptionOptions encryption;
  std::chrono::milliseconds timeout{10000};
  std::shared_ptr<livekit::sdk::detail::WakeEvent> work_wakeup =
      std::make_shared<livekit::sdk::detail::WakeEvent>();
  std::shared_ptr<livekit::sdk::detail::ReadySet<uint64_t>> cancellations =
      std::make_shared<livekit::sdk::detail::ReadySet<uint64_t>>(work_wakeup, 256);
  livekit::sdk::detail::DeadlineIndex<uint64_t> send_deadlines;
  uint64_t next_send = 0;
  std::map<uint64_t, Pending> outgoing;
  void erase_send_locked(std::map<uint64_t, Pending>::iterator);
  void expire_sends_locked();
  std::deque<DataEvent> incoming;
  size_t outgoing_bytes = 0, incoming_bytes = 0;
  // Worker-only packets already taken from RpcSession but awaiting native capacity.
  std::deque<livekit::DataPacket> rpc_pending;
  void emit(DataEvent);
  bool send_packet(const livekit::DataPacket&, const Send&);
};
}  // namespace livekit::sdk::rtc::detail
