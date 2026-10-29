#pragma once
#include "livekit/media_core/frame_view.hpp"
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace livekit::sdk::media {
// CPU-owned immutable payloads. All times are microseconds on the file timeline,
// normalized to the container start. Frames remain valid after seek/close.
struct VideoFrame {
  int width = 0, height = 0, stride = 0;
  int64_t timestamp_us = 0, duration_us = 0;
  uint64_t generation = 0;
  // Packed RGBA8, top to bottom; conversion is explicit in the FFmpeg node.
  std::shared_ptr<const std::vector<uint8_t>> rgba;
};

struct AudioFrame {
  int sample_rate = 48000, channels = 2, samples = 0;
  int64_t timestamp_us = 0, duration_us = 0;
  uint64_t generation = 0;
  // Interleaved float32 stereo, FL/FR.
  std::shared_ptr<const std::vector<float>> pcm;
};

// Zero-copy adapters expose the same immutable view contract as RTC outputs.
inline FrameView frame_view(const VideoFrame& frame) {
  FrameView view;
  view.owner = frame.rgba;
  view.format = FrameFormat::rgba8;
  view.width = frame.width;
  view.height = frame.height;
  if (frame.rgba) {
    view.planes = {std::as_bytes(std::span(*frame.rgba))};
  }
  view.strides = {size_t(frame.stride)};
  view.timing.timestamp = uint64_t(frame.timestamp_us);
  view.timing.clock_rate = 1000000;
  view.timing.epoch = frame.generation;
  return view;
}

inline FrameView frame_view(const AudioFrame& frame) {
  FrameView view;
  view.owner = frame.pcm;
  view.format = FrameFormat::pcm_f32;
  view.sample_rate = frame.sample_rate;
  view.channels = uint16_t(frame.channels);
  if (frame.pcm) {
    view.planes = {std::as_bytes(std::span(*frame.pcm))};
  }
  view.strides = {size_t(frame.channels) * sizeof(float)};
  view.timing.timestamp = uint64_t(frame.timestamp_us);
  view.timing.clock_rate = 1000000;
  view.timing.epoch = frame.generation;
  return view;
}

using Frame = std::variant<VideoFrame, AudioFrame>;
}  // namespace livekit::sdk::media
