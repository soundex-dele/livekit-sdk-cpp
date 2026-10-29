#pragma once
#include "livekit/rtc/audio_nodes.hpp"
#include "livekit/rtc/video_nodes.hpp"
#include <filesystem>

namespace livekit::sdk::rtc::nodes {
struct RecordingSnapshot {
  uint64_t admitted = 0, written = 0;
  size_t queued_frames = 0, queued_bytes = 0;
  std::string error;
};

// Per-track WAV/Y4M branch. Disk I/O runs on a bounded worker. Queue overflow is
// a branch error, never a silent hole in a supposedly complete recording.
// Y4M uses nominal 30 fps; these files do not preserve RTP timing or A/V sync.
// A new epoch or format starts a new file. Existing paths are never overwritten.
class RecordingNode final : public graphflow::core::GraphNode {
 public:
  RecordingNode(std::string name, std::filesystem::path directory, TrackId, MediaKind);
  ~RecordingNode() override;
  void quiesce();  // Close admission, drain queued writes and finalize WAV headers.
  RecordingSnapshot snapshot() const;
  void throw_if_failed() const;

 protected:
  bool doInit() override;
  bool doStart() override;
  void doStop() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk::rtc::nodes
