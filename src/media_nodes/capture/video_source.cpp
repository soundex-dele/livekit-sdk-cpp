#include "media_nodes/capture/video_source.hpp"
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace livekit::sdk {
struct I420VideoSource::Impl {
  explicit Impl(I420SourceOptions value) : options(value) {}
  const I420SourceOptions options;
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  VideoSourceStats stats;
  std::deque<std::shared_ptr<const detail::I420SourceFrame>> frames;
  size_t bytes() const { return static_cast<size_t>(options.width) * options.height * 3 / 2; }
  void discard() {
    frames.pop_front(); ++stats.dropped;
    stats.queued_bytes -= bytes(); stats.queued_frames = frames.size();
  }
  void clear() { while (!frames.empty()) discard(); }
  void expire() {
    const auto now = std::chrono::steady_clock::now();
    while (!frames.empty() && now - frames.front()->captured > options.max_age) discard();
  }
};
I420VideoSource::I420VideoSource(I420SourceOptions options) {
  if (options.width < 2 || options.width > 3840 || options.height < 2 || options.height > 2160 ||
      options.width % 2 || options.height % 2 || !options.max_frames || options.max_frames > 1024 ||
      options.max_bytes < static_cast<size_t>(options.width) * options.height * 3 / 2 ||
      options.max_bytes > 512u * 1024 * 1024 || options.max_age <= std::chrono::milliseconds::zero() ||
      options.max_age > std::chrono::seconds(5))
    throw std::invalid_argument("Invalid I420 source dimensions, queue capacity or frame age");
  impl_ = std::make_unique<Impl>(options);
}
I420VideoSource::~I420VideoSource() = default;
I420SourceOptions I420VideoSource::options() const { return impl_->options; }
VideoWriteStatus I420VideoSource::write(const VideoFrame& frame) {
  auto& s = *impl_; std::lock_guard lock(s.mutex);
  if (s.stats.closed) return VideoWriteStatus::closed;
  const auto& o = s.options;
  const auto valid = [](const std::vector<uint8_t>& pixels, int stride, int width, int height) {
    return stride >= width && pixels.size() >= static_cast<size_t>(stride) * (height - 1) + width;
  };
  if (frame.width != o.width || frame.height != o.height ||
      !valid(frame.y, frame.stride_y, o.width, o.height) ||
      !valid(frame.u, frame.stride_u, o.width / 2, o.height / 2) ||
      !valid(frame.v, frame.stride_v, o.width / 2, o.height / 2))
    throw std::invalid_argument("I420 source requires matching dimensions and complete positive-stride planes");
  s.expire();
  if (!s.stats.accepting || s.frames.size() >= o.max_frames || s.stats.queued_bytes > o.max_bytes - s.bytes()) {
    ++s.stats.rejected; return VideoWriteStatus::backpressure;
  }
  auto owned = std::make_shared<detail::I420SourceFrame>();
  owned->captured = std::chrono::steady_clock::now();
  auto& f = owned->frame;
  f.width = o.width; f.height = o.height;
  f.stride_y = o.width; f.stride_u = f.stride_v = o.width / 2;
  f.stream_generation = s.stats.generation;
  const auto copy = [](auto& to, const auto& from, int stride, int width, int height) {
    to.resize(static_cast<size_t>(width) * height);
    for (int row = 0; row < height; ++row)
      std::memcpy(to.data() + static_cast<size_t>(row) * width, from.data() + static_cast<size_t>(row) * stride, width);
  };
  copy(f.y, frame.y, frame.stride_y, o.width, o.height);
  copy(f.u, frame.u, frame.stride_u, o.width / 2, o.height / 2);
  copy(f.v, frame.v, frame.stride_v, o.width / 2, o.height / 2);
  s.frames.push_back(std::move(owned)); ++s.stats.accepted;
  s.stats.queued_bytes += s.bytes(); s.stats.queued_frames = s.frames.size();
  s.changed.notify_one(); return VideoWriteStatus::accepted;
}
VideoSourceStats I420VideoSource::stats() const { std::lock_guard lock(impl_->mutex); return impl_->stats; }
void I420VideoSource::close() {
  auto& s = *impl_; std::lock_guard lock(s.mutex);
  s.stats.closed = true; s.stats.accepting = false; s.clear(); s.changed.notify_all();
}
namespace detail {
I420SourceReader::I420SourceReader(std::shared_ptr<I420VideoSource> source) : source_(std::move(source)) {
  if (!source_) throw std::invalid_argument("I420 source is required");
  auto& s = *source_->impl_; std::lock_guard lock(s.mutex);
  if (s.stats.attached || s.stats.closed) throw std::invalid_argument("I420 source is already published or closed");
  s.clear(); s.stats.attached = true;
}
I420SourceReader::~I420SourceReader() {
  auto& s = *source_->impl_; std::lock_guard lock(s.mutex);
  s.stats.accepting = false; s.stats.attached = false; s.clear(); s.changed.notify_all();
}
void I420SourceReader::start() {
  auto& s = *source_->impl_; std::lock_guard lock(s.mutex);
  s.clear(); ++s.stats.generation; s.stats.accepting = !s.stats.closed; s.changed.notify_all();
}
void I420SourceReader::stop() {
  auto& s = *source_->impl_; std::lock_guard lock(s.mutex);
  s.stats.accepting = false; s.clear(); s.changed.notify_all();
}
std::shared_ptr<const I420SourceFrame> I420SourceReader::read(std::stop_token stop) {
  auto& s = *source_->impl_; std::unique_lock lock(s.mutex);
  for (;;) {
    s.expire();
    if (stop.stop_requested() || !s.stats.accepting) return {};
    if (!s.frames.empty()) {
      auto result = std::move(s.frames.front()); s.frames.pop_front(); ++s.stats.consumed;
      s.stats.queued_bytes -= s.bytes(); s.stats.queued_frames = s.frames.size(); return result;
    }
    s.changed.wait(lock, stop, [&] { return !s.stats.accepting || !s.frames.empty(); });
  }
}
}
}
