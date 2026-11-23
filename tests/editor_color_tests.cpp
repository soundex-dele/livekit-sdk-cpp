#include "../src/engines/editor/color_graph.hpp"
#include "test_failure.hpp"
#include <limits>

using namespace livekit::sdk;

int main() {
  // Two pixels with the same RGB but different alpha, plus a pixel that clips.
  const EditorImage source{3, 1, {100, 50, 10, 0, 100, 50, 10, 137, 255, 200, 255, 255}};
  const auto original = source.rgba;
  EditorColor color;
  color.temperature = 1;
  color.brightness = 1;
  color.saturation = 0;
  auto graph = editor_detail::ColorGraph::create(color);
  if (!graph) {
    test::fail(graph.error().message);
  }
  // Grade: [255,100,14], [255,100,14], [255,255,255]. Luma rounds to 127,127,255.
  const std::vector<uint8_t> expected{127, 127, 127, 0, 127, 127, 127, 137, 255, 255, 255, 255};
  for (int i = 0; i < 2; ++i) {
    auto rendered = (*graph)->apply(source);
    if (!rendered || (*rendered)->rgba != expected || source.rgba != original) {
      test::fail("grade must precede desaturation without changing alpha or source bytes");
    }
  }
  if ((*graph)->apply({1, 1, {0, 0, 0}})) {
    test::fail("truncated input accepted");
  }
  color.contrast = std::numeric_limits<double>::quiet_NaN();
  if (editor_detail::ColorGraph::create(color)) {
    test::fail("nonfinite color accepted");
  }
  return 0;
}
