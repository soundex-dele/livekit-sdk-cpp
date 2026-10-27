#pragma once
#include "livekit/video_stream.hpp"

namespace livekit::sdk::detail {
// Internal producer, shared only within one logical Room lifecycle.
class VideoFrameOutput {
 public:
  explicit VideoFrameOutput(std::shared_ptr<VideoFrameStream> stream);
  ~VideoFrameOutput();
  VideoFrameOutput(const VideoFrameOutput&) = delete;
  VideoFrameOutput& operator=(const VideoFrameOutput&) = delete;
  void submit(VideoFrame frame);
  void remove(const std::string& track_id);
  void advance(); // transport/room boundary; discard old-generation frames
  void close();
 private:
  std::shared_ptr<VideoFrameStream> stream_;
};
}
