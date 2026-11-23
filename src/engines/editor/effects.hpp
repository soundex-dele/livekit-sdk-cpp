#pragma once
#include "editor_internal.hpp"
#include "render_events.hpp"

namespace livekit::sdk::editor_detail {
bool render_text(EditorImage& image, const EditorText& text, std::string& error) noexcept;

// A complete candidate graph is built and started before replacing the live graph.
// Only immutable settings and reusable processor/cache state cross that boundary;
// bindings, emitters and INodes belong exclusively to one Graph.
class RenderPipeline {
 public:
  struct Callbacks {
    std::function<void(Work&)> decode, composite, mix, finish_audio;
  };

  static Result<std::unique_ptr<RenderPipeline>> build(const EditorProject& project,
                                                       const Callbacks& callbacks,
                                                       const RenderPipeline* previous);
  ~RenderPipeline();
  void render(const std::shared_ptr<Work>& work);
  void inspect(RenderGraphStats& stats) const;

 private:
  RenderPipeline();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk::editor_detail
