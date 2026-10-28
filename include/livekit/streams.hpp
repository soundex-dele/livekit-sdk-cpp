#pragma once
#include "livekit/operation.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace livekit::sdk {
namespace detail { class StreamSession; struct StreamReaderState; }
struct StreamOptions {
  std::string topic, stream_id, mime_type = "application/octet-stream", name;
  std::vector<std::string> destination_identities;
  std::map<std::string,std::string> attributes;
  std::optional<uint64_t> total_size;
  std::string reply_to_stream_id;
  std::vector<std::string> attached_stream_ids;
  int operation=0,version=0;
  bool generated=false;
};
struct StreamInfo {
  std::string id, sender_identity, topic, mime_type, name, reply_to_stream_id;
  std::vector<std::string> attached_stream_ids;
  int operation=0,version=0;
  bool generated=false;
  std::map<std::string,std::string> attributes;
  std::optional<uint64_t> total_size;
  int64_t timestamp_ms = 0;
  bool text = false;
};
// Reads may block only the caller. EOF is nullopt; timeout, cancellation and
// abnormal completion throw SDKError. An acquired chunk owns its storage.
class StreamReader {
 public:
  StreamInfo info() const;
  std::optional<std::vector<uint8_t>> read(std::chrono::milliseconds timeout = std::chrono::seconds(30), std::stop_token stop = {});
  std::vector<uint8_t> read_all(size_t max_bytes = 16 * 1024 * 1024, std::stop_token stop = {});
  std::string read_all_text(size_t max_bytes = 16 * 1024 * 1024, std::stop_token stop = {});
  void close();
 private:
  explicit StreamReader(std::shared_ptr<detail::StreamReaderState> state) : state_(std::move(state)) {}
  std::shared_ptr<detail::StreamReaderState> state_;
  friend class detail::StreamSession;
};
// Futures mean local channel submission, not remote delivery. Calls are ordered
// per writer. A bounded queue rejects an entire write with resource_exhausted;
// await the previous write before submitting another large block.
class StreamWriter {
 public:
  ~StreamWriter();
  const StreamInfo& info() const { return info_; }
  std::future<void> write(std::span<const uint8_t> bytes, const OperationOptions& operation = {});
  std::future<void> write(const std::string& text, const OperationOptions& operation = {});
  std::future<void> close(const std::string& reason = {}, const OperationOptions& operation = {});
 private:
  StreamWriter(std::weak_ptr<detail::StreamSession> session, uint64_t generation, StreamInfo info)
    : session_(std::move(session)), generation_(generation), info_(std::move(info)) {}
  std::weak_ptr<detail::StreamSession> session_;
  uint64_t generation_;
  StreamInfo info_;
  friend class detail::StreamSession;
};
using TextStreamReader = StreamReader;
using ByteStreamReader = StreamReader;
using TextStreamWriter = StreamWriter;
using ByteStreamWriter = StreamWriter;
// Runs outside the signaling thread. Consume the reader or hand it to another
// application thread; handlers must return after cancellation/reader closure.
using StreamHandler = std::function<void(std::shared_ptr<StreamReader>)>;
}
