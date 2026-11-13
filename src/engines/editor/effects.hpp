#pragma once
#include "render_events.hpp"
#include "engine/graph_builder.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace livekit::sdk::editor_detail {
// Publication is compiled at the external-library exception boundary.
class FlowNode : public graphflow::core::GraphNode {
 public:
  using GraphNode::GraphNode;
  bool send_layer(const Layer& layer, const std::string& route = "");
};

// A topology plan only. Nodes schedule downstream processing through graph edges.
class EffectNodes {
 public:
  static Result<std::unique_ptr<EffectNodes>> prepare(const EditorProject& project,
                                                      graphflow::core::EventBus* bus,
                                                      std::function<void(Work&)> finish_audio,
                                                      const EffectNodes* previous = nullptr);
  ~EffectNodes();
  void append_to(std::vector<std::shared_ptr<graphflow::core::GraphNode>>& nodes,
                 std::vector<graphflow::engine::NodeConnection>& connections) const;
  size_t reused_nodes() const;
  // Owner worker, after every synchronous frame, including errors. Releases any
  // backend event correlations whose branch failed to produce an output.
  void finish_frame();

 private:
  EffectNodes();
  class Impl;
  std::unique_ptr<Impl> impl_;
};

bool render_text(EditorImage& image, const EditorText& text, std::string& error) noexcept;
}  // namespace livekit::sdk::editor_detail
