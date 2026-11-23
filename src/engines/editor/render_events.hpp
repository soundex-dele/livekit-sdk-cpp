#pragma once
#include "livekit/editor.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace livekit::sdk::editor_detail {
// One frame transaction on the render worker. Track/asset references are valid
// until this synchronous graph invocation finishes; evaluated clips are owned.
struct TransitionFrame {
  std::string owner;
  std::shared_ptr<EditorFrame> output;
  std::array<std::shared_ptr<EditorFrame>, 2> inputs;
  double progress = 0;
  bool delivered = false;
};

struct Work {
  std::shared_ptr<EditorFrame> output;
  const EditorTrack* track = nullptr;
  std::shared_ptr<const EditorClip> clip;
  const EditorAsset* asset = nullptr;
  int64_t source_frame = 0;
  std::shared_ptr<const EditorImage> image, next_image;
  std::vector<float> audio;
  bool stretch_needed = false;
  int64_t audio_block = 0, audio_first = 0, target_sample = 0;
  int audio_count = 0;
  std::shared_ptr<TransitionFrame> transition;
  int transition_input = 0;
  Failure error;
  bool delivered = false;
};

inline std::string effect_route(const std::string& clip, const char* effect) {
  return "editor.clip." + std::to_string(clip.size()) + ":" + clip + "." + effect;
}
}  // namespace livekit::sdk::editor_detail
