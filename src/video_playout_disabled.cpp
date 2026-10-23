#include "lk/video_playout.hpp"
#include <stdexcept>
namespace lk {
struct VideoPlayout::Impl {};
VideoPlayout::VideoPlayout(EventEmitter) { throw std::runtime_error("Video playout is disabled; rebuild with LIVEKIT_ENABLE_VIDEO_PLAYOUT=ON"); }
VideoPlayout::~VideoPlayout() = default;
void VideoPlayout::add_track(const std::string&) {}
void VideoPlayout::remove_track(const std::string&) {}
void VideoPlayout::submit(const std::string&, const webrtc::VideoFrame&) {}
bool VideoPlayout::poll() { return false; }
}
