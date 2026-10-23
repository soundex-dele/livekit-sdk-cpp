#pragma once
#include "livekit/media.hpp"
#include "api/video/video_frame.h"
#include <map>
#include <mutex>
#include <optional>

namespace lk {
// Native producers never invoke application callbacks. Storage is bounded to
// one reference-counted native frame per attached track, not one per arrival.
class VideoFrames {
 public:
  explicit VideoFrames(livekit::sdk::VideoFrameHandler handler) : handler_(std::move(handler)) {}
  void add(const std::string& id);
  void remove(const std::string& id);
  void submit(const std::string& id, const webrtc::VideoFrame& frame);
  void dispatch(); // session thread only; propagates callback/conversion errors
 private:
  livekit::sdk::VideoFrameHandler handler_;
  std::mutex mutex_;
  std::map<std::string, std::optional<webrtc::VideoFrame>> latest_;
};
}
