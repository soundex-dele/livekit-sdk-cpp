#pragma once
#include "backends/rtc/livekit/service_result.hpp"
#include "livekit/data_track.hpp"
#include "livekit/encryption.hpp"
#include "livekit.pb.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <span>

namespace livekit::sdk::detail {
struct DataFrameQueue {
  mutable std::mutex mutex;
  std::condition_variable_any ready;
  std::deque<DataFrame> frames;
  size_t bytes = 0, capacity = 8;
  bool closed = false;
  Failure error;
  DataTrackStats stats;
  void close(Failure failure = {});
};

class DataTrackSession : public std::enable_shared_from_this<DataTrackSession> {
 public:
  void connect();
  Result<void> configure(EncryptionOptions encryption);
  void disconnect();
  void resume();
  void rejoin();
  std::vector<LocalDataTrack> local_tracks();
  void packet_sent(std::span<const uint8_t> packet, bool success);
  std::future<Result<LocalDataTrack>> publish(std::string name, DataTrackOptions options,
                                              OperationOptions operation);
  std::future<Result<void>> unpublish(uint64_t generation, uint16_t handle,
                                      OperationOptions operation);
  Result<DataTrackInfo> info(uint64_t generation, uint16_t handle);
  Result<DataTrackStats> stats(uint64_t generation, uint16_t handle);
  Result<void> write(uint64_t generation, uint16_t handle, DataFrame frame);
  std::vector<RemoteDataTrack> remote_tracks();
  std::future<Result<std::shared_ptr<DataTrackStream>>> subscribe(uint64_t generation,
                                                                  const std::string& sid,
                                                                  DataSubscribeOptions options,
                                                                  OperationOptions operation);
  void participant(const livekit::ParticipantInfo& participant);
  Result<void> receive_signal(const livekit::SignalResponse& response);
  Result<void> receive_packet(std::span<const uint8_t> packet);
  Result<bool> needs_publisher();
  Result<std::vector<livekit::SignalRequest>> take_signals();
  Result<std::optional<std::vector<uint8_t>>> take_packet();
  void sync(livekit::SyncState& state);

 private:
  using Clock = std::chrono::steady_clock;

  struct Pending {
    OperationOptions operation;
    Clock::time_point deadline;
    bool sent = false;
  };

  struct Local {
    livekit::DataTrackInfo wire;
    DataTrackInfo info;
    DataTrackStats stats;
    uint16_t sequence = 0, frame_number = 0;
    std::deque<std::vector<uint8_t>> sending, latest;
    std::shared_ptr<livekit::sdk::detail::ServicePromise<LocalDataTrack>> publish;
    std::shared_ptr<livekit::sdk::detail::ServicePromise<void>> unpublish;
    Pending pending;
    bool restoring = false;
  };

  struct Partial {
    uint16_t start = 0;
    bool has_start = false;
    std::optional<uint16_t> end;
    uint32_t timestamp = 0;
    std::optional<uint64_t> user_timestamp;
    std::map<uint16_t, std::vector<uint8_t>> chunks;
    size_t bytes = 0;
    Clock::time_point created = Clock::now();
    std::vector<uint8_t> encryption;
  };

  struct Waiter {
    std::shared_ptr<livekit::sdk::detail::ServicePromise<std::shared_ptr<DataTrackStream>>> promise;
    std::shared_ptr<DataFrameQueue> queue;
    Pending pending;
  };

  struct Remote {
    DataTrackInfo info;
    uint16_t handle = 0;
    bool subscribed = false;
    std::vector<std::weak_ptr<DataFrameQueue>> readers;
    std::vector<Waiter> waiters;
    std::deque<std::pair<uint16_t, uint32_t>> completed;
    Pending pending;
    std::optional<uint32_t> target_fps;
    std::map<uint16_t, Partial> partials;
    DataTrackStats stats;
  };

  Result<void> tick();
  void clear_remote(Remote& remote, Failure failure);
  void subscription_signal(const std::string& sid, bool subscribe,
                           std::optional<uint32_t> fps = {});
  mutable std::mutex mutex_;
  bool connected_ = false;
  uint64_t generation_ = 0;
  uint32_t next_handle_ = 0;
  std::map<uint16_t, Local> local_;
  std::map<std::string, Remote> remote_;
  std::map<uint16_t, std::string> handles_;
  std::deque<livekit::SignalRequest> signals_;
  uint16_t send_cursor_ = 0;
  EncryptionOptions encryption_;
  std::vector<Remote> restoring_remote_;
};
}  // namespace livekit::sdk::detail
