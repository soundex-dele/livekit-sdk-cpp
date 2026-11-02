#include "media_nodes/render/video_playout.hpp"

namespace lk {
using namespace livekit::sdk;
struct VideoPlayout::Impl {};

VideoPlayout::VideoPlayout(EventEmitter) : impl_(std::make_unique<Impl>()) {}
VideoPlayout::~VideoPlayout() = default;
void VideoPlayout::add_track(const std::string&) {}
void VideoPlayout::remove_track(const std::string&) {}
void VideoPlayout::submit(const std::string&, const webrtc::VideoFrame&) {}

Result<void> VideoPlayout::status() const {
  return Result<void>::failure(
      make_failure(ErrorDomain::media, ErrorCode::unsupported,
                   "Video playout is disabled; rebuild with LIVEKIT_ENABLE_VIDEO_PLAYOUT=ON"));
}

Result<bool> VideoPlayout::poll() {
  return Result<bool>::failure(status().failure_record());
}
}  // namespace lk
