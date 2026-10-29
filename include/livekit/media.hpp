#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>

namespace livekit::sdk {
class I420VideoSource;
class PcmAudioSource;
class EncodedMediaSource;
// Owned I420 pixels, rotated upright. Chroma dimensions round up for odd sizes.
struct VideoFrame {
  std::string track_id;
  int width = 0, height = 0;
  int stride_y = 0, stride_u = 0, stride_v = 0;
  int64_t timestamp_us = 0;
  std::vector<uint8_t> y, u, v;
  // Input source generation or decoded room epoch, as assigned by its node.
  uint64_t stream_generation = 0;
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
  // Application-fed live video. Mutually exclusive with input/demo/devices.
  std::shared_ptr<I420VideoSource> video_source;
  // Independent application PCM track; set video=false.
  std::shared_ptr<PcmAudioSource> audio_source;
  // Opt-in single-layer live-video demand control. The server's aggregate
  // OFF stops this graph's capture/encoder and RTP sender; ON restarts them.
  // Supported for application-fed video, generated video and capture devices,
  // never files/network playback (which must not restart from the beginning).
  bool dynacast = false;
  std::shared_ptr<EncodedMediaSource> encoded_source;
};
struct ScreenCaptureDevice {
  std::string id, name;
  // Physical pixel width/height. Origins use platform desktop coordinates
  // (logical points on macOS, physical pixels on Windows), possibly negative.
  int x = 0, y = 0, width = 0, height = 0;
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
