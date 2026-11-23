#pragma once
#include "internal.hpp"
#include <functional>

namespace livekit::sdk::studio_detail {
// Private Studio branch. The Studio owner admits media; a separate owner drains
// the graph and finalizes the file. Only shutdown/destruction waits for drain.
class RecordingSession {
 public:
  using Time = std::chrono::steady_clock::time_point;

  using AudioTrace = std::function<void(Time, const std::vector<float>&)>;

  struct Status {
    Failure first, cleanup;
    std::optional<Time> mediaOrigin;
    bool done = false;
    uint64_t admitted = 0;
  };

  RecordingSession(std::filesystem::path, const StudioOptions&, std::function<Time()> mediaOrigin,
                   std::function<void()>, AudioTrace = {});
  ~RecordingSession();
  void video(std::shared_ptr<const StudioImage>, Time);
  void audio(const std::vector<float>&, Time);
  void stop();
  Status status() const;
  void wait();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk::studio_detail
