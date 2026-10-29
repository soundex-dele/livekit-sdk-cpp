#pragma once
#include "livekit/media.hpp"
#include <chrono>
#include <memory>
#include <optional>

namespace livekit::sdk {
namespace detail {
class I420SourceReader;
}

struct I420SourceOptions {
  int width = 640, height = 360;
  size_t max_frames = 3;
  size_t max_bytes = 32 * 1024 * 1024;
  std::chrono::milliseconds max_age{200};
  bool dynamic_dimensions = false;  // Accept even sizes up to 3840x2160.
};
enum class VideoWriteStatus { accepted, backpressure, closed };
enum class VideoRotation { none = 0, clockwise90 = 90, clockwise180 = 180, clockwise270 = 270 };

struct RgbaInputFrame {
  int width = 0, height = 0, stride = 0;
  // Byte order R,G,B,A; alpha is ignored. Positive stride, CPU pixels.
  std::vector<uint8_t> pixels;
  VideoRotation rotation = VideoRotation::none;
};

struct VideoSourceStats {
  size_t queued_frames = 0, queued_bytes = 0;
  uint64_t accepted = 0, consumed = 0, dropped = 0, rejected = 0, generation = 0;
  bool attached = false, accepting = false, closed = false;
};

// Thread-safe CPU I420 input. One active publication at a time. write() copies
// visible pixels, ignores padding and stamps local monotonic capture time.
// VideoFrame timestamp_us/track_id/stream_generation are output metadata and
// are not used here. Feed live, upright frames at the desired capture rate.
class I420VideoSource {
 public:
  explicit I420VideoSource(I420SourceOptions options = {});
  ~I420VideoSource();
  I420VideoSource(const I420VideoSource&) = delete;
  I420VideoSource& operator=(const I420VideoSource&) = delete;
  I420SourceOptions options() const;
  // Never waits for queue space. A full queue or inactive publisher returns
  // backpressure; invalid dimensions/planes throw invalid_argument.
  VideoWriteStatus write(const VideoFrame& frame);
  // Explicit steady_clock capture time, strictly increasing per activation.
  // Outside now-5s..now+100ms is invalid; older than max_age is backpressure.
  VideoWriteStatus write(const VideoFrame& frame,
                         std::optional<std::chrono::steady_clock::time_point> captured);
  // Convert RGBA to BT.601 limited-range I420, then rotate clockwise. Resulting
  // dimensions must match options(). Conversion and copy occur on the caller.
  VideoWriteStatus write_rgba(const RgbaInputFrame& frame,
                              std::optional<std::chrono::steady_clock::time_point> captured = {});
  VideoSourceStats stats() const;
  // Permanent EOS: discard pending frames and wake the consumer. Publication
  // remains until explicitly unpublished (or Room exits).
  void close();

 private:
  friend class detail::I420SourceReader;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk
