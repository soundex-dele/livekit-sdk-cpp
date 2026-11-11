#pragma once
#include "livekit/editor.hpp"
#include <nlohmann/json.hpp>

namespace livekit::sdk::editor_detail {
inline nlohmann::json clip_extra(const EditorClip& c) {
  nlohmann::json keys = nlohmann::json::array();
  for (const auto& a : c.animations) {
    nlohmann::json frames = nlohmann::json::array();
    for (const auto& k : a.keys) {
      frames.push_back({k.frame, k.value, int(k.interpolation)});
    }
    keys.push_back({{"property", a.property}, {"keys", frames}});
  }
  return {{"rotation", c.rotation},
          {"speed", c.speed},
          {"fade_in", c.fade_in},
          {"fade_out", c.fade_out},
          {"text",
           {{"content", c.text.content},
            {"font", c.text.font},
            {"size", c.text.size},
            {"alignment", c.text.alignment},
            {"color", c.text.color},
            {"outline_color", c.text.outline_color},
            {"outline", c.text.outline}}},
          {"color",
           {{"brightness", c.color.brightness},
            {"contrast", c.color.contrast},
            {"saturation", c.color.saturation},
            {"temperature", c.color.temperature}}},
          {"transition",
           {{"next", c.transition.next_clip_id},
            {"effect", c.transition.effect},
            {"duration", c.transition.duration}}},
          {"animations", keys}};
}
}  // namespace livekit::sdk::editor_detail
