#pragma once
#include "livekit/studio.hpp"
#include <deque>
#include <mutex>
#include <span>

namespace livekit::sdk::studio_detail {
using Layers = std::vector<std::pair<StudioSource, std::shared_ptr<const StudioImage>>>;

class Compositor {
 public:
  Compositor(int width, int height, bool software);
  ~Compositor();
  StudioImage render(const Layers&);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Capture {
 public:
  explicit Capture(const StudioSource&);
  ~Capture();
  std::shared_ptr<const StudioImage> frame();
  std::string error();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class AudioCapture {
 public:
  explicit AudioCapture(bool system);
  ~AudioCapture();
  std::vector<float> take(size_t frames);
  std::string error();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Monitor {
 public:
  Monitor();
  ~Monitor();
  void write(std::span<const float>);
  void clear();
  std::string error();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Recorder {
 public:
  Recorder(const std::filesystem::path&, int width, int height, int fps);
  ~Recorder();
  void video(const StudioImage&, std::chrono::steady_clock::time_point);
  void audio(std::span<const float>, std::chrono::steady_clock::time_point);
  void finish();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::shared_ptr<StudioImage> load_image(const std::string&);
std::shared_ptr<StudioImage> text_image(const std::string&, const std::array<float, 4>&);
std::shared_ptr<StudioImage> from_i420(const VideoFrame&);
VideoFrame to_i420(const StudioImage&);
}  // namespace livekit::sdk::studio_detail
