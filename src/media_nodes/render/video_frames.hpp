#pragma once
#include "livekit/media.hpp"
#include "media_nodes/render/video_stream.hpp"
#include "api/video/video_frame.h"
#include <map>
#include <mutex>
#include <optional>

namespace lk {
// Native producers never invoke application callbacks. Storage is bounded to
// one reference-counted native frame per attached track, not one per arrival.
class VideoFrames {
 public:
  explicit VideoFrames(livekit::sdk::VideoFrameHandler handler,
      std::shared_ptr<livekit::sdk::detail::VideoFrameOutput> output = {}) : handler_(std::move(handler)), output_(std::move(output)) {}
  void add(const std::string& id);
  void remove(const std::string& id);
  void submit(const std::string& id, const webrtc::VideoFrame& frame);
  void dispatch(); // session thread only; propagates callback/conversion errors
  void reset(); // discard old-room native/pull frames before migration callback
 private:
  livekit::sdk::VideoFrameHandler handler_;
  std::shared_ptr<livekit::sdk::detail::VideoFrameOutput> output_;
  std::mutex mutex_;
  struct Slot { uint64_t incarnation; std::optional<webrtc::VideoFrame> frame; };
  uint64_t next_incarnation_ = 0;
  std::map<std::string, Slot> latest_;
};
}
