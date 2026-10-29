#include "media_nodes/capture/video_source.hpp"
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include "libyuv/convert.h"
#include "libyuv/rotate.h"

namespace livekit::sdk {
struct I420VideoSource::Impl {
  explicit Impl(I420SourceOptions value) : options(value) {}

  const I420SourceOptions options;
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  VideoSourceStats stats;
  std::optional<std::chrono::steady_clock::time_point> last_capture;
  std::deque<std::shared_ptr<const detail::I420SourceFrame>> frames;

  size_t bytes() const {
    return static_cast<size_t>(options.width) * options.height * 3 / 2;
  }

  void discard() {
    stats.queued_bytes -= frames.front()->frame.y.size() + frames.front()->frame.u.size() +
                          frames.front()->frame.v.size();
    frames.pop_front();
    ++stats.dropped;
    stats.queued_frames = frames.size();
  }

  void clear() {
    while (!frames.empty())
      discard();
  }

  void expire() {
    const auto now = std::chrono::steady_clock::now();
    while (!frames.empty() && now - frames.front()->captured > options.max_age)
      discard();
  }
};

I420VideoSource::I420VideoSource(I420SourceOptions options) {
  if (options.width < 2 || options.width > 3840 || options.height < 2 || options.height > 2160 ||
      options.width % 2 || options.height % 2 || !options.max_frames || options.max_frames > 1024 ||
      options.max_bytes < static_cast<size_t>(options.width) * options.height * 3 / 2 ||
      options.max_bytes > 512u * 1024 * 1024 ||
      options.max_age <= std::chrono::milliseconds::zero() ||
      options.max_age > std::chrono::seconds(5))
    throw std::invalid_argument("Invalid I420 source dimensions, queue capacity or frame age");
  impl_ = std::make_unique<Impl>(options);
}

I420VideoSource::~I420VideoSource() = default;

I420SourceOptions I420VideoSource::options() const {
  return impl_->options;
}

VideoWriteStatus I420VideoSource::write(const VideoFrame& frame) {
  return write(frame, {});
}

VideoWriteStatus I420VideoSource::write(
    const VideoFrame& frame, std::optional<std::chrono::steady_clock::time_point> captured) {
  auto& s = *impl_;
  std::lock_guard lock(s.mutex);
  if (s.stats.closed)
    return VideoWriteStatus::closed;
  const auto& o = s.options;
  const auto now = std::chrono::steady_clock::now();
  const auto timestamp = captured.value_or(
      s.last_capture ? std::max(now, *s.last_capture + std::chrono::microseconds(1)) : now);
  if (captured && (timestamp < now - std::chrono::seconds(5) ||
                   timestamp > now + std::chrono::milliseconds(100) ||
                   (s.last_capture && timestamp <= *s.last_capture)))
    throw std::invalid_argument("Invalid or non-monotonic video capture time");
  const auto valid = [](const std::vector<uint8_t>& pixels, int stride, int width, int height) {
    return stride >= width && pixels.size() >= static_cast<size_t>(stride) * (height - 1) + width;
  };
  const int width = frame.width, height = frame.height;
  const size_t bytes = width > 0 && height > 0 ? size_t(width) * height * 3 / 2 : 0;
  if (width < 2 || width > 3840 || height < 2 || height > 2160 || width % 2 || height % 2 ||
      (!o.dynamic_dimensions && (width != o.width || height != o.height)) ||
      !valid(frame.y, frame.stride_y, width, height) ||
      !valid(frame.u, frame.stride_u, width / 2, height / 2) ||
      !valid(frame.v, frame.stride_v, width / 2, height / 2))
    throw std::invalid_argument(
        "I420 source requires matching dimensions and complete positive-stride planes");
  s.expire();
  if (!s.stats.accepting || now - timestamp > o.max_age || s.frames.size() >= o.max_frames ||
      bytes > o.max_bytes || s.stats.queued_bytes > o.max_bytes - bytes) {
    ++s.stats.rejected;
    return VideoWriteStatus::backpressure;
  }
  auto owned = std::make_shared<detail::I420SourceFrame>();
  owned->captured = timestamp;
  auto& f = owned->frame;
  f.width = width;
  f.height = height;
  f.stride_y = width;
  f.stride_u = f.stride_v = width / 2;
  f.stream_generation = s.stats.generation;
  const auto copy = [](auto& to, const auto& from, int stride, int width, int height) {
    to.resize(static_cast<size_t>(width) * height);
    for (int row = 0; row < height; ++row)
      std::memcpy(to.data() + static_cast<size_t>(row) * width,
                  from.data() + static_cast<size_t>(row) * stride, width);
  };
  copy(f.y, frame.y, frame.stride_y, width, height);
  copy(f.u, frame.u, frame.stride_u, width / 2, height / 2);
  copy(f.v, frame.v, frame.stride_v, width / 2, height / 2);
  s.frames.push_back(std::move(owned));
  ++s.stats.accepted;
  s.last_capture = timestamp;
  s.stats.queued_bytes += bytes;
  s.stats.queued_frames = s.frames.size();
  s.changed.notify_one();
  return VideoWriteStatus::accepted;
}

VideoWriteStatus I420VideoSource::write_rgba(
    const RgbaInputFrame& input, std::optional<std::chrono::steady_clock::time_point> captured) {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stats.closed)
      return VideoWriteStatus::closed;
  }
  const auto rotation = static_cast<int>(input.rotation);
  if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
    throw std::invalid_argument("Invalid video rotation");
  const auto& o = impl_->options;
  const bool swap = rotation == 90 || rotation == 270;
  if (input.width < 2 || input.width > 3840 || input.height < 2 || input.height > 3840 ||
      input.width % 2 || input.height % 2 ||
      (!o.dynamic_dimensions && (input.width != (swap ? o.height : o.width) ||
                                 input.height != (swap ? o.width : o.height))) ||
      input.stride < input.width * 4 ||
      input.pixels.size() <
          static_cast<size_t>(input.stride) * (input.height - 1) + input.width * 4)
    throw std::invalid_argument("RGBA dimensions/stride/pixels do not match rotated source");
  auto allocate = [](int width, int height) {
    VideoFrame f;
    f.width = width;
    f.height = height;
    f.stride_y = width;
    f.stride_u = f.stride_v = width / 2;
    f.y.resize(static_cast<size_t>(width) * height);
    f.u.resize(f.y.size() / 4);
    f.v.resize(f.y.size() / 4);
    return f;
  };
  auto frame = allocate(input.width, input.height);
  // libyuv ABGR names packed words; on supported little-endian hosts bytes are RGBA.
  if (libyuv::ABGRToI420(input.pixels.data(), input.stride, frame.y.data(), frame.stride_y,
                         frame.u.data(), frame.stride_u, frame.v.data(), frame.stride_v,
                         input.width, input.height))
    throw std::runtime_error("RGBA conversion failed");
  if (!rotation)
    return write(frame, captured);
  auto upright = allocate(swap ? input.height : input.width, swap ? input.width : input.height);
  if (libyuv::I420Rotate(frame.y.data(), frame.stride_y, frame.u.data(), frame.stride_u,
                         frame.v.data(), frame.stride_v, upright.y.data(), upright.stride_y,
                         upright.u.data(), upright.stride_u, upright.v.data(), upright.stride_v,
                         frame.width, frame.height, static_cast<libyuv::RotationMode>(rotation)))
    throw std::runtime_error("I420 rotation failed");
  return write(upright, captured);
}

VideoSourceStats I420VideoSource::stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}

void I420VideoSource::close() {
  auto& s = *impl_;
  std::lock_guard lock(s.mutex);
  s.stats.closed = true;
  s.stats.accepting = false;
  s.clear();
  s.changed.notify_all();
}

namespace detail {
I420SourceReader::I420SourceReader(std::shared_ptr<I420VideoSource> source)
    : source_(std::move(source)) {
  if (!source_)
    throw std::invalid_argument("I420 source is required");
  auto& s = *source_->impl_;
  std::lock_guard lock(s.mutex);
  if (s.stats.attached || s.stats.closed)
    throw std::invalid_argument("I420 source is already published or closed");
  s.clear();
  s.stats.attached = true;
}

I420SourceReader::~I420SourceReader() {
  auto& s = *source_->impl_;
  std::lock_guard lock(s.mutex);
  s.stats.accepting = false;
  s.stats.attached = false;
  s.clear();
  s.changed.notify_all();
}

void I420SourceReader::start() {
  auto& s = *source_->impl_;
  std::lock_guard lock(s.mutex);
  s.clear();
  s.last_capture.reset();
  ++s.stats.generation;
  s.stats.accepting = !s.stats.closed;
  s.changed.notify_all();
}

void I420SourceReader::stop() {
  auto& s = *source_->impl_;
  std::lock_guard lock(s.mutex);
  s.stats.accepting = false;
  s.clear();
  s.changed.notify_all();
}

std::shared_ptr<const I420SourceFrame> I420SourceReader::read(std::stop_token stop) {
  auto& s = *source_->impl_;
  std::unique_lock lock(s.mutex);
  for (;;) {
    s.expire();
    if (stop.stop_requested() || !s.stats.accepting)
      return {};
    if (!s.frames.empty()) {
      auto result = std::move(s.frames.front());
      s.frames.pop_front();
      ++s.stats.consumed;
      s.stats.queued_bytes -=
          result->frame.y.size() + result->frame.u.size() + result->frame.v.size();
      s.stats.queued_frames = s.frames.size();
      return result;
    }
    s.changed.wait(lock, stop, [&] { return !s.stats.accepting || !s.frames.empty(); });
  }
}
}  // namespace detail
}  // namespace livekit::sdk
