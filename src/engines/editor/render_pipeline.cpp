#include "effects.hpp"
#include "color_graph.hpp"
#include <graphflow/graph.hpp>
#include <nndeploy/algorithm/phase_vocoder.h>
#include <nlohmann/json.hpp>
#include <deque>
#include <map>

namespace livekit::sdk::editor_detail {
namespace {
namespace gf = graphflow;
using Task = std::shared_ptr<Work>;
using Route = std::optional<std::string>;
using Operation = std::function<Route(Work&)>;
using Json = nlohmann::json;
constexpr gf::In<Task> input{"layer"};
constexpr gf::Out<Task> output{"processed"};

Operation transform(std::function<void(Work&)> process) {
  return [process = std::move(process)](Work& w) -> Route {
    process(w);
    return "next";
  };
}

class Stage final : public gf::INode {
 public:
  explicit Stage(std::shared_ptr<Operation> op) : op_(std::move(op)) {}

  void bind(gf::Binder& b) override {
    writer_ = b.output(output);
    b.input(input, [this](const Task& task) -> gf::Status {
      const auto route = (*op_)(*task);
      if (!route || task->error) {
        return {};
      }
      auto sent = writer_.emit(task, *route);
      if (sent.status == gf::Admission::Accepted && sent.accepted == 1) {
        return {};
      }
      if (sent.error.code != gf::ErrorCode::Ok) {
        return {std::move(sent.error)};
      }
      return {{gf::ErrorCode::DispatchFailure, "expected one receiver for " + *route}};
    });
  }

 private:
  std::shared_ptr<Operation> op_;
  gf::Emitter<Task> writer_;
};

struct TextCache {
  std::deque<std::shared_ptr<EditorImage>> frames;
  size_t bytes = 0;

  void retain(std::shared_ptr<EditorImage> image) {
    while (!frames.empty() &&
           (frames.size() >= 8 || bytes + image->rgba.size() > 128 * 1024 * 1024)) {
      bytes -= frames.front()->rgba.size();
      frames.pop_front();
    }
    bytes += image->rgba.size();
    frames.push_back(std::move(image));
  }
};

Operation text_op(EditorText text, std::shared_ptr<TextCache> cache) {
  return transform(
      [text = std::move(text), cache, saved = std::weak_ptr<EditorImage>{}](Work& w) mutable {
        if (w.track->hidden) {
          return;
        }
        auto frame = saved.lock();
        if (!frame || frame->width != w.output->image.width ||
            frame->height != w.output->image.height) {
          frame = std::make_shared<EditorImage>();
          frame->width = w.output->image.width;
          frame->height = w.output->image.height;
          frame->rgba.resize(size_t(frame->width) * frame->height * 4);
          std::string error;
          if (!render_text(*frame, text, error)) {
            w.error = failure(error);
            return;
          }
          saved = frame;
          cache->retain(frame);
        }
        w.image = std::move(frame);
      });
}

void stretch(Work& w, double rate) {
  if (!w.stretch_needed || w.audio.empty()) {
    return;
  }
  const auto count = w.audio.size() / 2;
  const auto length = size_t(std::llround(count / rate));
  if (!length || w.audio.size() % 2) {
    w.error = failure("time stretch: invalid stereo block");
    return;
  }
  std::vector<float> result(length * 2);
  std::vector<double> channel(count);
  for (size_t c = 0; c < 2; ++c) {
    for (size_t i = 0; i < count; ++i) {
      channel[i] = w.audio[i * 2 + c];
    }
    auto samples = nndeploy::dsp::stretch(channel, rate);
    for (size_t i = 0; i < length; ++i) {
      result[i * 2 + c] = float(samples[i]);
    }
  }
  w.audio = std::move(result);
}

Route join(Work& w) {
  auto& t = *w.transition;
  auto& slot = t.inputs[w.transition_input];
  if (slot) {
    w.error = failure("transition: duplicate input");
    return {};
  }
  slot = w.output;
  if (!t.inputs[0] || !t.inputs[1]) {
    return {};
  }
  w.image = std::shared_ptr<const EditorImage>(t.inputs[0], &t.inputs[0]->image);
  w.next_image = std::shared_ptr<const EditorImage>(t.inputs[1], &t.inputs[1]->image);
  w.output = t.output;
  w.audio.resize(w.output->audio.size());
  for (size_t i = 0; i < w.audio.size(); ++i) {
    w.audio[i] =
        float(t.inputs[0]->audio[i] * (1 - t.progress) + t.inputs[1]->audio[i] * t.progress);
  }
  return "next";
}

void transition(Work& w, const std::string& effect) {
  const auto& a = *w.image;
  const auto& b = *w.next_image;
  const auto t = w.transition->progress;
  auto frame = std::make_shared<EditorImage>(a);
  const int shift = int(std::round(t * a.width));
  for (int y = 0; y < a.height; ++y) {
    for (int x = 0; x < a.width; ++x) {
      const auto i = (size_t(y) * a.width + x) * 4;
      if (effect == "fade") {
        for (size_t c = 0; c < 3; ++c) {
          frame->rgba[i + c] = uint8_t(std::nearbyint(float(a.rgba[i + c]) * float(1 - t) +
                                                      float(b.rgba[i + c]) * float(t)));
        }
      } else if (effect == "slide") {
        const int offset = x + shift;
        const auto k = (size_t(y) * a.width + offset % a.width) * 4;
        const auto& source = offset < a.width ? a : b;
        std::copy_n(source.rgba.data() + k, 3, frame->rgba.data() + i);
      } else if ((x + .5) / a.width <= t) {
        std::copy_n(b.rgba.data() + i, 3, frame->rgba.data() + i);
      }
    }
  }
  w.image = std::move(frame);
}
}  // namespace

struct RenderPipeline::Impl {
  struct Processor {
    Json signature;
    std::shared_ptr<Operation> op;
  };

  struct Edge {
    std::string from, to, label;
  };

  gf::Graph graph{{gf::Dispatch::Inline}};
  gf::Emitter<Task> writer;
  std::map<std::string, Processor> processors;
  std::shared_ptr<TextCache> text_cache;
  size_t reused = 0;

  Result<void> configure(const EditorProject& project, const Callbacks& callbacks,
                         const Impl* old) {
    text_cache = old ? old->text_cache : std::make_shared<TextCache>();
    auto ingress = graph.ingress<Task>("editor.timeline");
    if (!ingress) {
      return Result<void>::failure(failure(ingress.status().error.message));
    }
    writer = ingress->writer;
    Failure error;
    std::map<std::string, gf::NodeRef<Stage>> refs;
    auto add = [&](const std::string& id, Json settings, auto factory) {
      if (error) {
        return;
      }
      std::shared_ptr<Operation> op;
      if (old) {
        auto it = old->processors.find(id);
        if (it != old->processors.end() && it->second.signature == settings) {
          op = it->second.op;
          if (id.compare(0, 12, "editor.clip.") == 0) {
            ++reused;
          }
        }
      }
      if (!op) {
        op = std::make_shared<Operation>(factory());
      }
      if (error) {
        return;
      }
      auto ref = graph.add<Stage>(id, op);
      if (!ref) {
        error = failure(id + ": " + ref.status().error.message);
        return;
      }
      refs.emplace(id, *ref);
      processors.emplace(id, Processor{std::move(settings), std::move(op)});
    };
    add("editor.decode", {}, [&] { return transform(callbacks.decode); });
    add("editor.composite", {}, [&] { return transform(callbacks.composite); });
    add("editor.mix", {}, [&] { return transform(callbacks.mix); });
    add("editor.route", {},
        [] { return [](Work& w) -> Route { return effect_route(w.clip->id, "pipeline"); }; });
    add("editor.result_route", {}, [] {
      return [](Work& w) -> Route {
        return w.transition ? effect_route(w.transition->owner, "transition") : "next";
      };
    });
    add("editor.output", {}, [] {
      return [](Work& w) -> Route {
        w.delivered = true;
        if (w.transition) {
          w.transition->delivered = true;
        }
        return {};
      };
    });
    std::vector<Edge> edges{{"editor.decode", "editor.route", "next"},
                            {"editor.composite", "editor.mix", "next"},
                            {"editor.mix", "editor.result_route", "next"},
                            {"editor.result_route", "editor.output", "next"}};
    for (const auto& track : project.tracks) {
      for (const auto& clip : track.clips) {
        std::string source = "editor.route", label = effect_route(clip.id, "pipeline");
        auto append = [&](const std::string& id) {
          edges.push_back({source, id, label});
          source = id;
          label = "next";
        };
        auto asset = std::find_if(project.assets.begin(), project.assets.end(),
                                  [&](const auto& a) { return a.id == clip.asset_id; });
        if (!clip.text.content.empty()) {
          const auto& t = clip.text;
          auto id = effect_route(clip.id, "text") + ".render";
          add(id, {t.content, t.font, t.size, t.alignment, t.color, t.outline_color, t.outline},
              [&] { return text_op(t, text_cache); });
          append(id);
        } else if (asset != project.assets.end()) {
          const auto& c = clip.color;
          if (asset->video &&
              (c.temperature != 0 || c.brightness != 0 || c.saturation != 1 || c.contrast != 1)) {
            const auto id = effect_route(clip.id, "color");
            add(id, {c.brightness, c.temperature, c.saturation, c.contrast}, [&]() -> Operation {
              auto built = ColorGraph::create(c);
              if (!built) {
                error = built.failure_record();
                return {};
              }
              return transform([color = std::shared_ptr<ColorGraph>(std::move(*built))](Work& w) {
                if (w.image) {
                  auto out = color->apply(*w.image);
                  if (out) {
                    w.image = std::move(*out);
                  } else {
                    w.error = out.failure_record();
                  }
                }
              });
            });
            append(id);
          }
          if (asset->audio && clip.speed != 1) {
            auto id = effect_route(clip.id, "audio");
            add(id + ".time_stretch", {clip.speed},
                [&] { return transform([rate = clip.speed](Work& w) { stretch(w, rate); }); });
            append(id + ".time_stretch");
            add(id + ".window", {}, [&] { return transform(callbacks.finish_audio); });
            append(id + ".window");
          }
          if (asset->audio && (clip.fade_in || clip.fade_out)) {
            auto id = effect_route(clip.id, "fade") + ".audio_fade";
            add(id, {project.rate.numerator, project.rate.denominator}, [&] {
              return transform(
                  [fps = double(project.rate.numerator) / project.rate.denominator](Work& w) {
                    const auto& c = *w.clip;
                    for (size_t i = 0; i < w.audio.size() / 2; ++i) {
                      const double frame = w.output->frame - c.position + double(i) * fps / 48000;
                      double gain = 1;
                      if (c.fade_in) {
                        gain *= std::clamp(frame / c.fade_in, 0., 1.);
                      }
                      if (c.fade_out) {
                        gain *= std::clamp((c.duration - frame) / c.fade_out, 0., 1.);
                      }
                      w.audio[i * 2] *= float(gain);
                      w.audio[i * 2 + 1] *= float(gain);
                    }
                  });
            });
            append(id);
          }
        }
        append("editor.composite");
        if (clip.transition.duration > 0) {
          const auto id = effect_route(clip.id, "transition");
          source = "editor.result_route";
          label = id;
          add(id + ".join", {}, [] { return join; });
          append(id + ".join");
          add(id + ".transition", {clip.transition.effect}, [&] {
            return transform([kind = clip.transition.effect](Work& w) { transition(w, kind); });
          });
          append(id + ".transition");
          add(id + ".commit", {}, [] {
            return transform([](Work& w) {
              w.output->image = *w.image;
              for (size_t i = 0; i < w.audio.size(); ++i) {
                w.output->audio[i] += w.audio[i];
              }
            });
          });
          append(id + ".commit");
          append("editor.output");
        }
      }
    }
    if (error) {
      return Result<void>::failure(std::move(error));
    }
    auto wired = graph.rewire([&](gf::Wiring& w) {
      w.connect(ingress->port, refs.find("editor.decode")->second.in(input));
      for (const auto& edge : edges) {
        w.connect(refs.find(edge.from)->second.out(output), refs.find(edge.to)->second.in(input),
                  edge.label);
      }
    });
    if (!wired) {
      return Result<void>::failure(failure("render graph wiring: " + wired.error.message));
    }
    if (auto started = graph.start(); !started) {
      return Result<void>::failure(failure("render graph start: " + started.error.message));
    }
    return Result<void>::success();
  }
};

RenderPipeline::RenderPipeline() : impl_(std::make_unique<Impl>()) {}

RenderPipeline::~RenderPipeline() = default;

Result<std::unique_ptr<RenderPipeline>> RenderPipeline::build(const EditorProject& project,
                                                              const Callbacks& callbacks,
                                                              const RenderPipeline* previous) {
  using Built = Result<std::unique_ptr<RenderPipeline>>;
  auto candidate = std::unique_ptr<RenderPipeline>(new RenderPipeline);
  auto status =
      candidate->impl_->configure(project, callbacks, previous ? previous->impl_.get() : nullptr);
  if (!status) {
    return Built::failure(status.failure_record());
  }
  return Built::success(std::move(candidate));
}

void RenderPipeline::render(const Task& work) {
  auto sent = impl_->writer.emit(work);
  if (sent.status != gf::Admission::Accepted && !work->error) {
    work->error = failure("render dispatch: " + sent.error.node + ": " + sent.error.message);
  }
}

void RenderPipeline::inspect(RenderGraphStats& stats) const {
  auto snap = impl_->graph.snapshot();
  if (!snap) {
    return;
  }
  stats.reused_effect_nodes = impl_->reused;
  for (const auto& node : snap->nodes) {
    const auto it = impl_->processors.find(node.id);
    RenderGraphStats::Node info{
        node.id,
        it == impl_->processors.end() ? 0 : reinterpret_cast<uintptr_t>(it->second.op.get()),
        {}};
    for (const auto& e : snap->edges) {
      if (e.from == node.id) {
        info.outputs.push_back(e.from + "->" + e.to);
      }
    }
    stats.nodes.push_back(std::move(info));
  }
}
}  // namespace livekit::sdk::editor_detail
