#pragma once
#include "media_graph/video_events.hpp"
#include "media_graph/realtime_channel.hpp"
#include "api/video/video_frame.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <variant>
namespace lk {
// Optional recording branch; disk I/O never executes on WebRTC callbacks.
// Overflow is a reported branch error rather than blocking the RTC producer.
class RecordingNode final : public livekit::sdk::detail::MediaNode {
 public:
  RecordingNode(const std::filesystem::path& directory, const std::string& id, bool video);
  ~RecordingNode() override;
  void submit(const webrtc::VideoFrame& frame);
  void submit(const void* data, int bits, int rate, size_t channels, size_t frames);
  std::string error() const;
 protected:
  bool doInit() override;
  bool doStart() override;
  void doStop() override;
 private:
  struct AudioBlock { int rate; size_t channels, frames; std::vector<int16_t> data; };
  using Item = std::variant<webrtc::VideoFrame, AudioBlock>;
  void run();
  void fail(const std::string& message);
  void write_video(const webrtc::VideoFrame& frame);
  void write_audio(const void* data, int bits, int rate, size_t channels, size_t frames);
  void open_video(int width, int height);
  void close_audio();
  livekit::sdk::detail::RealtimeChannel<Item> queue_{32, 64 * 1024 * 1024, false};
  std::thread thread_;
  bool video_;
  std::filesystem::path base_;
  std::ofstream file_;
  mutable std::mutex mutex_;
  std::string error_;
  uint64_t bytes_ = 0;
  int width_ = 0, height_ = 0, rate_ = 0, channels_ = 0, part_ = 0;
};
}
