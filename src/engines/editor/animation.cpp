#include "livekit/editor.hpp"
#include <algorithm>
#include <cmath>

namespace livekit::sdk {
namespace {
double* property(EditorClip& c, const std::string& name) {
  if (name == "x") {
    return &c.x;
  }
  if (name == "y") {
    return &c.y;
  }
  if (name == "scale") {
    return &c.scale;
  }
  if (name == "rotation") {
    return &c.rotation;
  }
  if (name == "opacity") {
    return &c.opacity;
  }
  if (name == "volume") {
    return &c.volume;
  }
  return nullptr;
}
}  // namespace

EditorClip editor_evaluate(const EditorClip& clip, int64_t frame) {
  auto result = clip;
  for (const auto& a : clip.animations) {
    auto* value = property(result, a.property);
    if (!value || a.keys.empty()) {
      continue;
    }
    auto next = std::upper_bound(a.keys.begin(), a.keys.end(), frame,
                                 [](int64_t f, const EditorKeyframe& k) { return f < k.frame; });
    if (next == a.keys.begin()) {
      *value = next->value;
    } else if (next == a.keys.end()) {
      *value = a.keys.back().value;
    } else {
      const auto& prev = *(next - 1);
      double t = double(frame - prev.frame) / (next->frame - prev.frame);
      if (prev.interpolation == EditorInterpolation::smooth) {
        t = t * t * (3 - 2 * t);
      }
      *value = prev.value + (next->value - prev.value) * t;
    }
  }
  return result;
}

void editor_retime(EditorClip& clip, int64_t offset, double scale) {
  if (!std::isfinite(scale) || scale <= 0) {
    return;
  }
  auto first = editor_evaluate(clip, offset);
  auto last = editor_evaluate(clip, offset + int64_t(std::llround(clip.duration / scale)));
  for (auto& a : clip.animations) {
    if (!property(first, a.property)) {
      continue;
    }
    auto interpolation = EditorInterpolation::linear;
    for (const auto& k : a.keys) {
      if (k.frame <= offset) {
        interpolation = k.interpolation;
      }
    }
    std::vector<EditorKeyframe> keys{{0, *property(first, a.property), interpolation}};
    for (auto k : a.keys) {
      k.frame = int64_t(std::llround((k.frame - offset) * scale));
      if (k.frame > 0 && k.frame < clip.duration) {
        if (keys.back().frame == k.frame) {
          keys.back() = k;
        } else {
          keys.push_back(k);
        }
      }
    }
    keys.push_back({clip.duration, *property(last, a.property)});
    a.keys = std::move(keys);
  }
  clip.fade_in =
      std::clamp<int64_t>(int64_t(std::llround((clip.fade_in - offset) * scale)), 0, clip.duration);
  clip.fade_out =
      std::min(int64_t(std::llround(clip.fade_out * scale)), clip.duration - clip.fade_in);
}
}  // namespace livekit::sdk
