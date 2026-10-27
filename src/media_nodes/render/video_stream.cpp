#include "media_nodes/render/video_stream.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace livekit::sdk {
struct VideoFrameStream::Impl {
  explicit Impl(VideoStreamOptions value) : options(value) {}
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  VideoStreamOptions options;
  VideoStreamStats stats;
  bool attached = false;
  std::deque<std::shared_ptr<const VideoFrame>> frames;
  static size_t bytes(const VideoFrame& frame) { return frame.y.size() + frame.u.size() + frame.v.size(); }
  void discard_front() {
    stats.queued_bytes -= bytes(*frames.front()); frames.pop_front(); ++stats.dropped;
    stats.queued_frames = frames.size();
  }
  void discard_all() { while (!frames.empty()) discard_front(); }
};
VideoFrameStream::VideoFrameStream(VideoStreamOptions options) {
  if (!options.max_frames || options.max_frames > 1024 || !options.max_bytes || options.max_bytes > 512u * 1024 * 1024)
    throw std::invalid_argument("Video stream requires 1..1024 frames and 1..512 MiB byte capacity");
  impl_ = std::make_unique<Impl>(options);
}
VideoFrameStream::~VideoFrameStream() = default;
VideoReadResult VideoFrameStream::read(std::chrono::milliseconds timeout, std::stop_token stop) {
  if (timeout < std::chrono::milliseconds::zero() || timeout > std::chrono::hours(24))
    throw std::invalid_argument("Video read timeout must be between zero and 24 hours");
  auto& s = *impl_; std::unique_lock lock(s.mutex);
  s.changed.wait_for(lock, stop, timeout, [&] { return s.stats.closed || !s.frames.empty(); });
  if (stop.stop_requested()) return {VideoReadStatus::cancelled, {}};
  if (s.stats.closed) return {VideoReadStatus::closed, {}};
  if (s.frames.empty()) return {VideoReadStatus::timeout, {}};
  auto frame = std::move(s.frames.front()); s.frames.pop_front();
  s.stats.queued_bytes -= Impl::bytes(*frame); s.stats.queued_frames = s.frames.size();
  return {VideoReadStatus::frame, std::move(frame)};
}
VideoStreamStats VideoFrameStream::stats() const { std::lock_guard lock(impl_->mutex); return impl_->stats; }
void VideoFrameStream::close() {
  auto& s = *impl_; std::lock_guard lock(s.mutex);
  s.stats.closed = true; s.discard_all(); s.changed.notify_all();
}
namespace detail {
VideoFrameOutput::VideoFrameOutput(std::shared_ptr<VideoFrameStream> stream) : stream_(std::move(stream)) {
  if (!stream_) throw std::invalid_argument("Video output requires a stream");
  auto& s = *stream_->impl_; std::lock_guard lock(s.mutex);
  if (s.attached || s.stats.closed) throw std::invalid_argument("Video stream is already attached or closed");
  s.attached = true;
}
VideoFrameOutput::~VideoFrameOutput() { close(); }
void VideoFrameOutput::submit(VideoFrame frame) {
  auto& s = *stream_->impl_; std::lock_guard lock(s.mutex);
  if (s.stats.closed) return;
  ++s.stats.received;
  const auto bytes = VideoFrameStream::Impl::bytes(frame);
  if (bytes > s.options.max_bytes) { ++s.stats.dropped; return; }
  while (!s.frames.empty() && (s.frames.size() >= s.options.max_frames || s.stats.queued_bytes > s.options.max_bytes - bytes))
    s.discard_front();
  frame.stream_generation = s.stats.generation;
  s.frames.push_back(std::make_shared<const VideoFrame>(std::move(frame)));
  s.stats.queued_bytes += bytes; s.stats.queued_frames = s.frames.size(); s.changed.notify_one();
}
void VideoFrameOutput::remove(const std::string& id) {
  auto& s = *stream_->impl_; std::lock_guard lock(s.mutex);
  for (auto it = s.frames.begin(); it != s.frames.end();) {
    if ((*it)->track_id == id) { s.stats.queued_bytes -= VideoFrameStream::Impl::bytes(**it); ++s.stats.dropped; it = s.frames.erase(it); }
    else ++it;
  }
  s.stats.queued_frames = s.frames.size();
}
void VideoFrameOutput::advance() {
  auto& s = *stream_->impl_; std::lock_guard lock(s.mutex);
  if (s.stats.closed) return;
  s.discard_all(); ++s.stats.generation;
}
void VideoFrameOutput::close() { stream_->close(); }
}
}
