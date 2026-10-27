#include "media_nodes/render/video_frames.hpp"
#include "api/video/i420_buffer.h"
#include <cstring>
#include <stdexcept>

namespace lk {
void VideoFrames::add(const std::string& id) { std::lock_guard lock(mutex_); latest_.try_emplace(id, Slot{++next_incarnation_, {}}); }
void VideoFrames::remove(const std::string& id) {
  std::lock_guard lock(mutex_); latest_.erase(id);
  if (output_) output_->remove(id);
}
void VideoFrames::reset() {
  std::lock_guard lock(mutex_);
  for (auto& [id, slot] : latest_) { slot.frame.reset(); slot.incarnation = ++next_incarnation_; }
  if (output_) output_->advance();
}
void VideoFrames::submit(const std::string& id, const webrtc::VideoFrame& frame) {
  std::lock_guard lock(mutex_);
  auto it = latest_.find(id); if (it != latest_.end()) it->second.frame = frame;
}
void VideoFrames::dispatch() {
  // Take one track at a time so removal of another track during a callback
  // cannot leave its stale frame in a detached batch.
  std::vector<std::string> ids;
  { std::lock_guard lock(mutex_); for (const auto& [id, slot] : latest_) if (slot.frame) ids.push_back(id); }
  for (const auto& id : ids) {
    std::optional<webrtc::VideoFrame> native;
    uint64_t incarnation = 0;
    { std::lock_guard lock(mutex_); auto it = latest_.find(id);
      if (it == latest_.end()) continue; native.swap(it->second.frame); incarnation = it->second.incarnation; }
    if (!native) continue;
    auto buffer = native->video_frame_buffer()->ToI420();
    if (!buffer) throw std::runtime_error("Cannot convert remote video to I420");
    if (native->rotation() != webrtc::kVideoRotation_0)
      buffer = webrtc::I420Buffer::Rotate(*buffer, native->rotation());
    livekit::sdk::VideoFrame out;
    out.track_id = id; out.width = buffer->width(); out.height = buffer->height();
    out.timestamp_us = native->timestamp_us();
    out.stride_y = out.width; out.stride_u = out.stride_v = (out.width + 1) / 2;
    auto copy = [](std::vector<uint8_t>& dst, const uint8_t* src, int pitch, int width, int height) {
      dst.resize(static_cast<size_t>(width) * height);
      for (int row = 0; row < height; ++row) std::memcpy(dst.data() + static_cast<size_t>(row) * width, src + row * pitch, width);
    };
    copy(out.y, buffer->DataY(), buffer->StrideY(), out.width, out.height);
    copy(out.u, buffer->DataU(), buffer->StrideU(), out.stride_u, (out.height + 1) / 2);
    copy(out.v, buffer->DataV(), buffer->StrideV(), out.stride_v, (out.height + 1) / 2);
    // Removal may race conversion on another native thread. Do not enqueue a
    // converted frame for a track removed while ToI420/copy was in flight.
    if (output_) {
      std::lock_guard lock(mutex_);
      const auto current = latest_.find(id);
      if (current != latest_.end() && current->second.incarnation == incarnation) output_->submit(std::move(out));
    } else if (handler_) handler_(out);
  }
}
}
