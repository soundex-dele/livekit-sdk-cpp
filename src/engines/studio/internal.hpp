#pragma once
#include "livekit/compat/span.hpp"
#include "livekit/studio.hpp"
#include "studio_result.hpp"
#include <deque>
#include <mutex>

namespace livekit::sdk::studio_detail {
using Layers = std::vector<std::pair<StudioSource, std::shared_ptr<const StudioImage>>>;

Result<void> validate_layers(const Layers& layers);
Result<std::string> path8(const std::filesystem::path& path);

class Compositor {
 public:
  Compositor(int width, int height, bool software);
  ~Compositor();
  Result<void> status() const;
  Result<StudioImage> render(const Layers&);

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
  void write(livekit::compat::span<const float>);
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
  Result<void> status() const;
  Result<void> video(const StudioImage&, std::chrono::steady_clock::time_point);
  Result<void> audio(livekit::compat::span<const float>, std::chrono::steady_clock::time_point);
  Result<void> finish();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

Result<std::shared_ptr<StudioImage>> load_image(const std::string&);
Result<std::shared_ptr<StudioImage>> text_image(const std::string&, const std::array<float, 4>&);
Result<std::shared_ptr<StudioImage>> from_i420(const VideoFrame&);
Result<VideoFrame> to_i420(const StudioImage&);
}  // namespace livekit::sdk::studio_detail
