#include "color_graph.hpp"
#include "editor_internal.hpp"
#include <graphflow/graph.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace livekit::sdk::editor_detail {
namespace {
namespace gf = graphflow;
// Exclusive working copy, used synchronously by the two stages in order.
using Pixels = std::shared_ptr<EditorImage>;
using Output = Result<std::shared_ptr<const EditorImage>>;
constexpr gf::In<Pixels> input_port{"rgba", gf::Connections::ExactlyOne};
constexpr gf::Out<Pixels> graded_port{"graded", gf::Connections::ExactlyOne};

gf::Status delivered(const gf::EmitResult& result) {
  if (result.status == gf::Admission::Accepted && result.accepted == 1) {
    return {};
  }
  if (result.error.code != gf::ErrorCode::Ok) {
    return {result.error};
  }
  return {{gf::ErrorCode::DispatchFailure, "color stage did not deliver to its receiver"}};
}

class Grade final : public gf::INode {
 public:
  explicit Grade(const EditorColor& color) {
    const double gains[]{1 + color.temperature * .3, 1., 1 - color.temperature * .3};
    const auto exposure = std::pow(2., color.brightness);
    // Match ColorGrade's uint8 truncation *before* adjusting saturation/contrast.
    for (size_t c = 0; c < table_.size(); ++c) {
      for (size_t i = 0; i < table_[c].size(); ++i) {
        table_[c][i] =
            static_cast<uint8_t>(std::clamp(i / 255. * gains[c] * exposure, 0., 1.) * 255.);
      }
    }
  }

  void bind(gf::Binder& binder) override {
    output_ = binder.output(graded_port);
    binder.input(input_port, [this](const Pixels& pixels) {
      auto& rgba = pixels->rgba;
      for (size_t i = 0; i < rgba.size(); i += 4) {
        for (size_t c = 0; c < table_.size(); ++c) {
          rgba[i + c] = table_[c][rgba[i + c]];
        }
      }
      return delivered(output_.emit(pixels));
    });
  }

 private:
  std::array<std::array<uint8_t, 256>, 3> table_{};
  gf::Emitter<Pixels> output_;
};

class SaturationContrast final : public gf::INode {
 public:
  explicit SaturationContrast(const EditorColor& color)
      : saturation_(color.saturation), contrast_(color.contrast) {}

  void bind(gf::Binder& binder) override {
    binder.input(input_port, [this](const Pixels& pixels) {
      auto& rgba = pixels->rgba;
      for (size_t i = 0; i < rgba.size(); i += 4) {
        const double gray = .2126 * rgba[i] + .7152 * rgba[i + 1] + .0722 * rgba[i + 2];
        for (size_t c = 0; c < 3; ++c) {
          const auto value =
              (gray + (rgba[i + c] - gray) * saturation_ - 127.5) * contrast_ + 127.5;
          rgba[i + c] = static_cast<uint8_t>(std::nearbyint(std::clamp(value, 0., 255.)));
        }
      }
    });
  }

 private:
  double saturation_, contrast_;
};
}  // namespace

struct ColorGraph::Impl {
  gf::Graph graph{{gf::Dispatch::Inline}};
  gf::Emitter<Pixels> writer;

  gf::Status build(const EditorColor& color) {
    auto source = graph.ingress<Pixels>("color.input");
    if (!source) {
      return source.status();
    }
    auto grade = graph.add<Grade>("color.grade", color);
    if (!grade) {
      return grade.status();
    }
    auto saturation = graph.add<SaturationContrast>("color.saturation_contrast", color);
    if (!saturation) {
      return saturation.status();
    }
    auto wired = graph.rewire([&](gf::Wiring& w) {
      w.connect(source->port, grade->in(input_port));
      w.connect(grade->out(graded_port), saturation->in(input_port));
    });
    if (!wired) {
      return wired;
    }
    writer = source->writer;
    return graph.start();
  }
};

ColorGraph::ColorGraph() : impl_(std::make_unique<Impl>()) {}

ColorGraph::~ColorGraph() = default;

Result<std::unique_ptr<ColorGraph>> ColorGraph::create(const EditorColor& color) {
  if (!valid_color(color)) {
    return Result<std::unique_ptr<ColorGraph>>::failure(
        failure("color graph: invalid color settings", ErrorCode::invalid_argument));
  }
  auto graph = std::unique_ptr<ColorGraph>(new ColorGraph());
  if (auto status = graph->impl_->build(color); !status) {
    return Result<std::unique_ptr<ColorGraph>>::failure(
        failure("color graph build: " + status.error.node + ": " + status.error.message));
  }
  return Result<std::unique_ptr<ColorGraph>>::success(std::move(graph));
}

Output ColorGraph::apply(const EditorImage& input) {
  if (input.width <= 0 || input.height <= 0 ||
      uint64_t(input.width) * input.height * 4 != input.rgba.size()) {
    return Output::failure(
        failure("color graph: invalid RGBA dimensions", ErrorCode::invalid_argument));
  }
  auto pixels = std::make_shared<EditorImage>(input);
  if (auto status = delivered(impl_->writer.emit(pixels)); !status) {
    return Output::failure(
        failure("color graph render: " + status.error.node + ": " + status.error.message));
  }
  return Output::success(std::move(pixels));
}
}  // namespace livekit::sdk::editor_detail
