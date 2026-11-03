#pragma once
#include "livekit/media_graph.hpp"
#include <algorithm>

namespace livekit::sdk::examples {
// Copy before editing: other graph branches may still own the original pixels.
class WatermarkNode final : public graphflow::core::GraphNode {
 public:
  explicit WatermarkNode(std::string name) : GraphNode(std::move(name), nullptr) {}

 protected:
  bool doInit() override {
    subscribe<rtc::nodes::VideoFrameEvent>([this](const auto& event) {
      if (!event.pixels) {
        return;
      }
      auto pixels = std::make_shared<VideoFrame>(*event.pixels);
      const auto width = std::min(16, pixels->width), height = std::min(16, pixels->height);
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          const auto at = static_cast<size_t>(y) * pixels->stride_y + x;
          if (at < pixels->y.size()) {
            pixels->y[at] = 235;
          }
        }
      }
      auto transformed = event;
      transformed.pixels = std::move(pixels);
      publish(transformed);
    });
    return true;
  }
};

inline std::shared_ptr<const client::MediaGraphOptions> watermark_graph() {
  auto options = std::make_shared<client::MediaGraphOptions>();
  options->build = [](client::MediaGraphContext context) {
    const auto source = context.publishing ? context.input : context.codec;
    const auto type = graphflow::core::detail::prettyName<rtc::nodes::VideoFrameEvent>();
    auto graph = std::move(context.defaults);
    bool connected = false;
    for (auto& edge : graph.connections) {
      if (edge.from == source && edge.dataType == type) {
        edge.from = "watermark";
        connected = true;
      }
    }
    if (!connected) {
      return Result<client::MediaGraph>::failure(make_failure(
          ErrorDomain::media, ErrorCode::invalid_argument, "Watermark requires raw video"));
    }
    graph.nodes.push_back(std::make_shared<WatermarkNode>("watermark"));
    graph.connect<rtc::nodes::VideoFrameEvent>(source, "watermark");
    return Result<client::MediaGraph>::success(std::move(graph));
  };
  return options;
}
}  // namespace livekit::sdk::examples
