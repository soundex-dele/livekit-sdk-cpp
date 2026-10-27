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
enum class CaptureKind { input, camera, screen };
struct MediaSpec {
  std::string input;
  std::string ffmpeg = "ffmpeg";
  bool video = true, demo = false, loop = false;
  int width = 640, height = 360, fps = 30;
  CaptureKind capture = CaptureKind::input;
  int device_index = 0; // camera enumeration index
  bool local_preview = false;
  std::string screen_id; // empty preserves legacy primary-display behavior
};
struct ScreenCaptureDevice {
  std::string id, name;
  int x = 0, y = 0, width = 0, height = 0; // physical desktop pixels, possibly negative origin
  bool primary = false;
  int rotation = 0, refresh_rate = 0;
  bool operator==(const ScreenCaptureDevice&) const = default;
};
struct CaptureSize { int width, height; };
std::vector<ScreenCaptureDevice> screen_capture_devices();
// Fit landscape into 1920x1080 or portrait into 1080x1920, without upscaling.
CaptureSize screen_capture_size(int width, int height);
struct VideoCaptureDevice { std::string name, id; };
std::vector<VideoCaptureDevice> video_capture_devices();
// probe_media also accepts camera://<index> and screen://primary without FFmpeg.
// File/network inputs require external ffprobe and ffmpeg. Generated sources do not.
std::vector<MediaSpec> probe_media(const std::string& input,
    const std::string& ffmpeg = "ffmpeg", bool loop = false,
    int width = 640, int height = 360, int fps = 30);
}
