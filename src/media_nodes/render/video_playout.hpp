#pragma once
#include "api/video/video_frame.h"
#include "engines/rtc/events.hpp"
#include <memory>
#include <string>

namespace lk {
// Construct, poll and destroy on the main thread. The track methods are safe
// on WebRTC callback threads; only the most recent frame per track is retained.
class VideoPlayout {
 public:
  explicit VideoPlayout(EventEmitter events = EventEmitter{});
  ~VideoPlayout();
  VideoPlayout(const VideoPlayout&) = delete;
  VideoPlayout& operator=(const VideoPlayout&) = delete;
  void add_track(const std::string& id);
  void remove_track(const std::string& id);
  void submit(const std::string& id, const webrtc::VideoFrame& frame);
  // False means the user closed a video window or pressed Escape.
  bool poll();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
