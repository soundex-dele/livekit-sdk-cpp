#pragma once
#include "livekit/streams.hpp"
#include "livekit.pb.h"
#include "support/task_executor.hpp"
#include "support/compression.hpp"
#include <deque>
#include <mutex>

namespace livekit::sdk::detail {
struct Utf8State {
  uint32_t value = 0, minimum = 0;
  unsigned remaining = 0;
  bool append(std::span<const uint8_t> bytes);
};

struct StreamReaderState {
  std::mutex mutex;
  std::condition_variable_any ready;
  StreamInfo info;
  std::deque<std::vector<uint8_t>> chunks;
  size_t bytes = 0;
  uint64_t index = 0, received = 0;
  Utf8State utf8;
  std::unique_ptr<InflateRaw> inflater;
  bool closed = false;
  std::exception_ptr error;
  std::chrono::steady_clock::time_point touched = std::chrono::steady_clock::now();
  void finish(std::exception_ptr failure = {});
};

class StreamSession : public std::enable_shared_from_this<StreamSession> {
 public:
  StreamSession() = default;
  ~StreamSession();
  void connect();
  void disconnect();
  void participant_left(const std::string& identity);
  void register_handler(bool text, const std::string& topic, StreamHandler handler);
  void unregister_handler(bool text, const std::string& topic);
  std::shared_ptr<StreamWriter> open(bool text, StreamOptions options);
  std::future<StreamInfo> send_text(std::string text, StreamOptions options,
                                    OperationOptions operation);
  std::future<StreamInfo> send_file(std::filesystem::path path, StreamOptions options,
                                    OperationOptions operation);
  std::future<void> enqueue(uint64_t generation, const std::string& id,
                            std::span<const uint8_t> bytes, std::optional<std::string> reason,
                            OperationOptions operation);
  void receive(const livekit::DataPacket& packet);
  bool has_outgoing();
  // Called only on the session runner; send is invoked without mutex_.
  void pump(const std::function<bool(const livekit::DataPacket&)>& send, size_t max_packets = 4,
            const std::function<bool()>& ready = {});

 private:
  struct Batch {
    std::deque<livekit::DataPacket> packets;
    std::shared_ptr<std::promise<void>> promise;
    OperationOptions options;
    std::chrono::steady_clock::time_point deadline;
    bool sent = false;
  };

  struct Outgoing {
    StreamInfo info;
    std::vector<std::string> destinations;
    std::deque<std::shared_ptr<Batch>> batches;
    uint64_t index = 0, size = 0;
    size_t queued = 0;
    bool closing = false;
    Utf8State utf8;
    std::chrono::steady_clock::time_point touched = std::chrono::steady_clock::now();
  };

  void expire();
  void fail_outgoing(const std::string& id, std::exception_ptr failure);
  std::mutex mutex_;
  uint64_t generation_ = 0;
  bool connected_ = false;
  std::map<std::pair<bool, std::string>, StreamHandler> handlers_;
  std::map<std::pair<std::string, std::string>, std::shared_ptr<StreamReaderState>> readers_;
  std::map<std::string, Outgoing> writers_;
  std::string cursor_;
  size_t queued_ = 0;
  TaskExecutor handlers_executor_{4, 64};
  TaskExecutor send_executor_{2, 16};
};
}  // namespace livekit::sdk::detail
