#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace livekit::sdk {
namespace detail {
class EncodedSourceReader;
class EncodedOutput;
}  // namespace detail

struct EncodedMediaFrame {
  std::vector<uint8_t> data;
  std::string codec, track_id;
  bool keyframe = false;
  int width = 0, height = 0;
  std::chrono::steady_clock::time_point captured{};
  std::chrono::microseconds duration{20000};
  uint32_t rtp_timestamp = 0;
  uint64_t generation = 0;
};

struct EncodedSourceOptions {
  std::string codec = "h264";
  int width = 640, height = 360;
  size_t max_frames = 32, max_bytes = 16 * 1024 * 1024;
  std::chrono::milliseconds max_age{500};
};
enum class EncodedWriteStatus { accepted, backpressure, closed, keyframe_required };

struct EncodedMediaStats {
  uint64_t accepted = 0, consumed = 0, dropped = 0, generation = 0;
  size_t queued_frames = 0, queued_bytes = 0;
  bool accepting = false, closed = false, keyframe_required = true;
  uint32_t target_bitrate = 0;
};

class EncodedMediaSource {
 public:
  explicit EncodedMediaSource(EncodedSourceOptions options = {});
  ~EncodedMediaSource();
  EncodedSourceOptions options() const;
  EncodedWriteStatus write(EncodedMediaFrame frame);
  EncodedMediaStats stats() const;
  // Signal a lost access unit: discard queued reference frames and require a keyframe.
  void request_keyframe();
  void close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend class detail::EncodedSourceReader;
};

using EncodedVideoSource = EncodedMediaSource;
using EncodedAudioSource = EncodedMediaSource;

class EncodedMediaStream {
 public:
  explicit EncodedMediaStream(size_t max_frames = 64, size_t max_bytes = 16 * 1024 * 1024);
  ~EncodedMediaStream();
  std::shared_ptr<const EncodedMediaFrame> read(
      std::chrono::milliseconds timeout = std::chrono::seconds(30), std::stop_token stop = {});
  void close();
  EncodedMediaStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend class detail::EncodedOutput;
};
}  // namespace livekit::sdk
