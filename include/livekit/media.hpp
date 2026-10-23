#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace livekit::sdk {
// Owned I420 pixels, rotated upright. Chroma dimensions round up for odd sizes.
struct VideoFrame {
  std::string track_id;
  int width = 0, height = 0;
  int stride_y = 0, stride_u = 0, stride_v = 0;
  int64_t timestamp_us = 0;
  std::vector<uint8_t> y, u, v;
};
using VideoFrameHandler = std::function<void(const VideoFrame&)>;
struct MediaSpec {
  std::string input;
  std::string ffmpeg = "ffmpeg";
  bool video = true, demo = false, loop = false;
  int width = 640, height = 360, fps = 30;
};
// File/network inputs require external ffprobe and ffmpeg. Generated sources do not.
std::vector<MediaSpec> probe_media(const std::string& input,
    const std::string& ffmpeg = "ffmpeg", bool loop = false,
    int width = 640, int height = 360, int fps = 30);
}
