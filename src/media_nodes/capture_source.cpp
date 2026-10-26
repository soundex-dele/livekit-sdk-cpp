#include "media_nodes/capture_source.hpp"
#include "media_nodes/capture.hpp"
#include "media_graph/video_events.hpp"
#include "lk/process.hpp"
#include "api/video/i420_buffer.h"
#include "rtc_base/time_utils.h"
#include <cmath>
#include <cstring>
#include <numbers>

namespace lk {
static std::vector<std::string> input_args(std::string input) {
  for (const auto& codec : {"h264", "h265", "vp8", "opus"}) {
    auto prefix = std::string(codec) + "://";
    if (input.starts_with(prefix)) {
      auto address = input.substr(prefix.size());
      auto transport = address.starts_with('/') ? "unix:" + address : "tcp://" + address;
      auto format = std::string(codec) == "h265" ? "hevc" : std::string(codec) == "vp8" ? "ivf" : std::string(codec) == "opus" ? "ogg" : "h264";
      return {"-probesize", "32768", "-analyzeduration", "0", "-f", format, "-i", transport};
    }
  }
  return {"-i", input};
}
CaptureNode::CaptureNode(MediaSpec value)
    : MediaNode(value.capture == livekit::sdk::CaptureKind::camera ? "CameraCaptureNode" :
                value.capture == livekit::sdk::CaptureKind::screen ? "ScreenCaptureNode" :
                value.video ? "VideoSourceNode" : "AudioSourceNode", livekit::sdk::detail::NodeExecution::source), spec(std::move(value)) {
  if (spec.width < 2 || spec.height < 2 || spec.width > 8192 || spec.height > 8192 ||
      spec.width % 2 || spec.height % 2 || spec.fps < 1 || spec.fps > 240 ||
      (spec.capture != livekit::sdk::CaptureKind::input && !spec.video))
    throw std::invalid_argument("Invalid capture format (even dimensions 2..8192, fps 1..240 required)");
}
CaptureNode::~CaptureNode() { release(); }
bool CaptureNode::doStart() {
  stop_ = false; finished_ = false; ++generation;
  { std::lock_guard lock(mutex_); error_.clear(); }
  return true;
}
void CaptureNode::activate() {
  if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
}
void CaptureNode::doStop() {
  stop_ = true;
  { std::lock_guard lock(mutex_); if (process_) process_->terminate(); }
  if (thread_.joinable()) thread_.join();
}
void CaptureNode::deliver(const webrtc::VideoFrame& frame) {
  ++inputs; publish(livekit::sdk::detail::RawVideoFrame(frame)); ++outputs;
}
std::string CaptureNode::error() const { std::lock_guard lock(mutex_); return error_; }
void CaptureNode::run() {
  try {
    if (spec.capture != livekit::sdk::CaptureKind::input) {
      capture_device(spec, stop_, [this](const webrtc::VideoFrame& frame) { deliver(frame); ++frames; });
      finished_ = true; return;
    }
    std::shared_ptr<Process> child;
    if (!spec.demo) {
      std::vector<std::string> args{spec.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-threads", "1", "-filter_threads", "1", "-rw_timeout", "10000000"};
      if (spec.loop) args.insert(args.end(), {"-stream_loop", "-1"});
      auto in = input_args(spec.input); args.insert(args.end(), in.begin(), in.end());
      if (spec.video) args.insert(args.end(), {"-map", "0:v:0", "-an", "-vf", "scale=" + std::to_string(spec.width) + ":" + std::to_string(spec.height) + ":force_original_aspect_ratio=decrease,pad=" + std::to_string(spec.width) + ":" + std::to_string(spec.height) + ":(ow-iw)/2:(oh-ih)/2,fps=" + std::to_string(spec.fps), "-pix_fmt", "yuv420p", "-f", "rawvideo", "pipe:1"});
      else args.insert(args.end(), {"-map", "0:a:0", "-vn", "-ac", "1", "-ar", "48000", "-f", "s16le", "pipe:1"});
      std::lock_guard lock(mutex_);
      if (stop_) { finished_ = true; return; }
      process_ = child = std::make_shared<Process>(args);
    }
    const auto period = std::chrono::microseconds(spec.video ? 1000000 / spec.fps : 10000);
    auto next = std::chrono::steady_clock::now();
    std::vector<uint8_t> pixels(static_cast<size_t>(spec.width) * spec.height * 3 / 2);
    int16_t samples[480]{};
    while (!stop_) {
      if (spec.video) {
        if (child && !child->read_exact(pixels.data(), pixels.size())) break;
        auto buffer = webrtc::I420Buffer::Create(spec.width, spec.height);
        if (child) {
          for (int row = 0; row < spec.height; ++row) std::memcpy(buffer->MutableDataY() + row * buffer->StrideY(), pixels.data() + row * spec.width, spec.width);
          for (int row = 0; row < spec.height / 2; ++row) {
            std::memcpy(buffer->MutableDataU() + row * buffer->StrideU(), pixels.data() + spec.width * spec.height + row * spec.width / 2, spec.width / 2);
            std::memcpy(buffer->MutableDataV() + row * buffer->StrideV(), pixels.data() + spec.width * spec.height * 5 / 4 + row * spec.width / 2, spec.width / 2);
          }
        } else {
          for (int y = 0; y < spec.height; ++y) for (int x = 0; x < spec.width; ++x) buffer->MutableDataY()[y * buffer->StrideY() + x] = static_cast<uint8_t>(16 + ((x + y + frames.load() * 3) % 220));
          std::memset(buffer->MutableDataU(), 96, buffer->StrideU() * spec.height / 2);
          std::memset(buffer->MutableDataV(), 160, buffer->StrideV() * spec.height / 2);
        }
        deliver(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_timestamp_us(webrtc::TimeMicros()).build());
      } else {
        if (child) {
          size_t used = 0;
          while (used < sizeof(samples)) { auto count = child->read(reinterpret_cast<char*>(samples) + used, sizeof(samples) - used); if (!count) break; used += count; }
          if (!used) break;
          if (used % 2) throw std::runtime_error("Truncated PCM sample");
          // A finite audio file need not end on a 10 ms boundary.
          std::memset(reinterpret_cast<char*>(samples) + used, 0, sizeof(samples) - used);
        }
        if (!child) for (int i = 0; i < 480; ++i) samples[i] = static_cast<int16_t>(6000 * std::sin(2 * std::numbers::pi * 440 * (frames.load() * 480 + i) / 48000.0));
        ++inputs; publish(livekit::sdk::detail::PcmFrame(samples, 16, 48000, 1, 480)); ++outputs;
      }
      ++frames;
      next += period;
      if (next < std::chrono::steady_clock::now() - period * 5) next = std::chrono::steady_clock::now();
      std::this_thread::sleep_until(next);
    }
    if (child && !stop_ && child->wait() != 0) throw std::runtime_error("FFmpeg decoder exited with an error");
    if (!stop_ && frames == 0) throw std::runtime_error("Input produced no media frames");
  } catch (const std::exception& e) { if (!stop_) { ++errors; std::lock_guard lock(mutex_); error_ = e.what(); } }
  finished_ = true;
}
}
