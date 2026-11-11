#pragma once
#include "livekit/editor.hpp"

namespace livekit::sdk::editor_detail {
// Exception boundary: all failures returned as text, no backend types escape.
// Owned by one render session and used only on its worker thread.
class EffectSession {
 public:
  EffectSession();
  ~EffectSession();
  EffectSession(const EffectSession&) = delete;
  EffectSession& operator=(const EffectSession&) = delete;
  bool grade(EditorImage& image, const EditorColor& color, std::string& error) noexcept;
  bool transition(EditorImage& image, const EditorImage& next, const std::string& effect,
                  double progress, std::string& error) noexcept;
  bool stretch_audio(std::vector<float>& audio, double rate, std::string& error) noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

bool render_text(EditorImage& image, const EditorText& text, std::string& error) noexcept;
}  // namespace livekit::sdk::editor_detail
