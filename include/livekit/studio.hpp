#pragma once
#include "livekit/video_source.hpp"
#include "livekit/audio_source.hpp"
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace livekit::sdk {
enum class StudioSourceKind { color, screen, window, camera, image, text, video, remote };

struct StudioTransform {
  float x = 0, y = 0, width = 1, height = 1;  // normalized output canvas coordinates
  float rotation = 0, crop_left = 0, crop_top = 0, crop_right = 0, crop_bottom = 0;
};

struct StudioChroma {
  bool enabled = false;
  std::array<float, 3> color{0, 1, 0};
  float similarity = .25f, smoothness = .1f, spill = .3f;
};

struct StudioSource {
  std::string id, name, input;
  StudioSourceKind kind = StudioSourceKind::color;
  StudioTransform transform;
  StudioChroma chroma;
  std::array<float, 4> color{.08f, .10f, .16f, 1};
  bool visible = true, locked = false, loop = true, muted = false;
  float volume = 1;
};

struct StudioScene {
  std::string id = "main", name = "主场景";
  std::vector<StudioSource> sources;
};

struct StudioOptions {
  int width = 1280, height = 720, fps = 30;
  bool software_gpu = false;
};

struct StudioAudioSettings {
  bool microphone = false, system = false, monitor = false;
  float microphone_volume = 1, system_volume = 1, media_volume = 1, remote_volume = 1;
};

struct StudioImage {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
};

struct StudioSnapshot {
  uint64_t frames = 0, dropped = 0;
  float microphone_peak = 0, system_peak = 0, output_peak = 0;
  bool recording = false;
  std::string error, recording_path;
  std::map<std::string, std::string> source_errors;
  std::shared_ptr<const StudioImage> preview;
};

// Owns all capture and media resources on a worker; UI calls never process media.
// Program includes remote guests. Local output excludes all remote media for
// interaction rooms, preventing cyclic picture/audio when two studios connect.
class Studio {
 public:
  explicit Studio(StudioOptions options = {});
  ~Studio();
  Studio(const Studio&) = delete;
  Studio& operator=(const Studio&) = delete;
  void set_scene(StudioScene scene);
  void set_audio(StudioAudioSettings settings);
  void submit_remote_video(std::string id, const VideoFrame& frame);
  void submit_remote_audio(std::string id, const AudioFrame& frame);
  void clear_remote();
  void remove_remote(const std::string& track_id);
  void start_recording(std::filesystem::path mp4_path);
  void stop_recording();
  StudioSnapshot snapshot() const;
  StudioOptions options() const;
  std::shared_ptr<I420VideoSource> video_output() const;
  std::shared_ptr<PcmAudioSource> audio_output() const;
  std::shared_ptr<I420VideoSource> local_video_output() const;
  std::shared_ptr<PcmAudioSource> local_audio_output() const;
  // Versioned project JSON; replace via same-directory atomic rename.
  static void save_project(const std::filesystem::path&, const std::vector<StudioScene>&,
                           size_t selected, StudioOptions, StudioAudioSettings);
  static std::vector<StudioScene> load_project(const std::filesystem::path&, size_t& selected,
                                               StudioOptions&, StudioAudioSettings&);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct StudioDevice {
  std::string id, name;
};

std::vector<StudioDevice> studio_windows();
std::vector<StudioDevice> studio_screens();
std::vector<StudioDevice> studio_cameras();
// Public pure operations for offline verification and media consumers.
StudioImage studio_composite_cpu(
    int width, int height,
    const std::vector<std::pair<StudioSource, std::shared_ptr<const StudioImage>>>& layers);
void studio_mix(std::vector<float>& target, const std::vector<float>& source, float gain);
}  // namespace livekit::sdk
