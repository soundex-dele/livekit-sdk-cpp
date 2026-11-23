#pragma once
#include "livekit/editor.hpp"

namespace livekit::sdk::editor_detail {
// Created, rendered and destroyed on the render worker. The decoded input is immutable.
class ColorGraph {
 public:
  static Result<std::unique_ptr<ColorGraph>> create(const EditorColor& color);
  ~ColorGraph();
  Result<std::shared_ptr<const EditorImage>> apply(const EditorImage& input);

 private:
  ColorGraph();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk::editor_detail
