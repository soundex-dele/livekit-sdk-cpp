#pragma once
#include "livekit/video_source.hpp"
#include <stop_token>

namespace livekit::sdk::detail {
struct I420SourceFrame {
  VideoFrame frame;
  std::chrono::steady_clock::time_point captured;
};

// RAII exclusive consumer. Reserved before AddTrack; activated only once the
// downstream graph is ready. A new binding never sees the previous queue.
class I420SourceReader {
 public:
  explicit I420SourceReader(std::shared_ptr<I420VideoSource> source);
  ~I420SourceReader();
  I420SourceReader(const I420SourceReader&) = delete;
  I420SourceReader& operator=(const I420SourceReader&) = delete;
  void start();
  void stop();
  std::shared_ptr<const I420SourceFrame> read(std::stop_token stop);

 private:
  std::shared_ptr<I420VideoSource> source_;
};
}  // namespace livekit::sdk::detail
