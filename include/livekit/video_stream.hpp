#pragma once
#include "livekit/media.hpp"
#include <chrono>
#include <memory>
#include <stop_token>

namespace livekit::sdk {
namespace detail { class VideoFrameOutput; }
struct VideoStreamOptions {
  size_t max_frames = 8;
  size_t max_bytes = 32 * 1024 * 1024;
};
enum class VideoReadStatus { frame, timeout, cancelled, closed };
struct VideoReadResult {
  VideoReadStatus status = VideoReadStatus::timeout;
  std::shared_ptr<const VideoFrame> frame;
};
struct VideoStreamStats {
  size_t queued_frames = 0, queued_bytes = 0;
  uint64_t received = 0, dropped = 0, generation = 0;
  bool closed = false;
};
// Single-session, thread-safe pull output. A stream may be attached to one Room
// once. Read frames own their pixels and remain valid after close/destruction.
// Full queues discard oldest complete frames; producer never waits for readers.
class VideoFrameStream {
 public:
  explicit VideoFrameStream(VideoStreamOptions options = {});
  ~VideoFrameStream();
  VideoFrameStream(const VideoFrameStream&) = delete;
  VideoFrameStream& operator=(const VideoFrameStream&) = delete;
  // A zero timeout polls. Cancellation takes precedence over queued frames.
  VideoReadResult read(std::chrono::milliseconds timeout, std::stop_token stop = {});
  VideoStreamStats stats() const;
  // Discard queued frames and wake readers. Does not disconnect the Room.
  void close();
 private:
  friend class detail::VideoFrameOutput;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
