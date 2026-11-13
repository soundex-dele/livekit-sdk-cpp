#include "effects.hpp"
#include "editor_internal.hpp"
#include <nndeploy/algorithm/algorithms.h>
#include <opencv2/imgproc.hpp>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <deque>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
}

namespace livekit::sdk::editor_detail {
bool FlowNode::send_layer(const Layer& layer, const std::string& route) {
  return publish(layer, route);
}

namespace {
using namespace nndeploy::flow;
using Node = graphflow::core::GraphNode;
using Nodes = std::vector<std::shared_ptr<Node>>;
using Connections = std::vector<graphflow::engine::NodeConnection>;

struct Context {
  uint64_t sequence = 0;
  std::map<uint64_t, std::shared_ptr<Work>> pending;
  std::deque<std::shared_ptr<EditorImage>> text_images;
  size_t text_bytes = 0;

  void retain_text(const std::shared_ptr<EditorImage>& image) {
    while (!text_images.empty() &&
           (text_images.size() >= 8 || text_bytes + image->rgba.size() > 128 * 1024 * 1024)) {
      text_bytes -= text_images.front()->rgba.size();
      text_images.pop_front();
    }
    text_bytes += image->rgba.size();
    text_images.push_back(image);
  }

  uint64_t put(const std::shared_ptr<Work>& work) {
    pending.emplace(++sequence, work);
    return sequence;
  }

  std::shared_ptr<Work> take(uint64_t sequence) {
    auto found = pending.find(sequence);
    if (found == pending.end()) {
      return {};
    }
    auto work = std::move(found->second);
    pending.erase(found);
    return work;
  }
};

MediaFrameEvent event(const EditorImage& image) {
  MediaFrameEvent e;
  e.session_id = "freecut";
  e.image = std::make_shared<cv::Mat>();
  cv::Mat rgba(image.height, image.width, CV_8UC4, const_cast<uint8_t*>(image.rgba.data()));
  cv::cvtColor(rgba, *e.image, cv::COLOR_RGBA2RGB);
  return e;
}

void copy_rgb(EditorImage& image, const cv::Mat& rgb) {
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      for (int c = 0; c < 3; ++c) {
        image.rgba[(size_t(y) * image.width + x) * 4 + c] = rgb.at<cv::Vec3b>(y, x)[c];
      }
    }
  }
}

class RgbaToRgb final : public Node {
 public:
  RgbaToRgb(const std::string& id, graphflow::core::EventBus* bus, std::shared_ptr<Context> ctx)
      : Node(id, bus), ctx_(std::move(ctx)) {}

 protected:
  bool doInit() override {
    subscribe<Layer>([this](const auto& layer) {
      const auto& w = *layer.work;
      if (w.error || !w.image) {
        publish(layer, "bypass");
        return;
      }
      auto e = event(*w.image);
      e.frame_id = ctx_->put(layer.work);
      if (w.next_image) {
        e.reference = event(*w.next_image).image;
        e.timestamp = w.transition->progress;
      }
      publish(e);
    });
    return true;
  }

 private:
  std::shared_ptr<Context> ctx_;
};

class RgbToRgba final : public Node {
 public:
  RgbToRgba(const std::string& id, graphflow::core::EventBus* bus, std::shared_ptr<Context> ctx)
      : Node(id, bus), ctx_(std::move(ctx)) {}

 protected:
  bool doInit() override {
    subscribe<MediaFrameEvent>([this](const auto& e) {
      if (e.session_id != "freecut") {
        return;
      }
      auto w = ctx_->take(e.frame_id);
      if (!w) {
        return;
      }
      if (!e.image || e.image->type() != CV_8UC3 || e.image->cols != w->image->width ||
          e.image->rows != w->image->height) {
        w->error = editor_detail::failure("RGB output shape/type mismatch");
      } else {
        auto image = std::make_shared<EditorImage>(*w->image);
        copy_rgb(*image, *e.image);
        w->image = std::move(image);
      }
      Layer layer;
      layer.work = std::move(w);
      publish(layer);
    });
    return true;
  }

 private:
  std::shared_ptr<Context> ctx_;
};

class FloatToAudio final : public Node {
 public:
  FloatToAudio(const std::string& id, graphflow::core::EventBus* bus, std::shared_ptr<Context> ctx)
      : Node(id, bus), ctx_(std::move(ctx)) {}

 protected:
  bool doInit() override {
    subscribe<Layer>([this](const auto& layer) {
      const auto& w = *layer.work;
      if (w.error || w.audio.empty() || !w.stretch_needed) {
        publish(layer, "bypass");
        return;
      }
      AudioEvent e;
      e.session_id = "freecut";
      e.frame_id = ctx_->put(layer.work);
      e.sample_rate = 48000;
      e.samples.shape = {w.audio.size() / 2, 2};
      e.samples.data.assign(w.audio.begin(), w.audio.end());
      publish(e);
    });
    return true;
  }

 private:
  std::shared_ptr<Context> ctx_;
};

class AudioToFloat final : public Node {
 public:
  AudioToFloat(const std::string& id, graphflow::core::EventBus* bus, std::shared_ptr<Context> ctx)
      : Node(id, bus), ctx_(std::move(ctx)) {}

 protected:
  bool doInit() override {
    subscribe<AudioEvent>([this](const auto& e) {
      if (e.session_id != "freecut") {
        return;
      }
      auto w = ctx_->take(e.frame_id);
      if (!w) {
        return;
      }
      if (e.sample_rate != 48000 || e.samples.shape.size() != 2 || e.samples.cols() != 2 ||
          e.samples.data.size() != e.samples.rows() * 2) {
        w->error = editor_detail::failure("audio output shape/rate mismatch");
      } else {
        w->audio.resize(e.samples.data.size());
        std::transform(e.samples.data.begin(), e.samples.data.end(), w->audio.begin(),
                       [](double value) { return float(value); });
      }
      Layer layer;
      layer.work = std::move(w);
      publish(layer);
    });
    return true;
  }

 private:
  std::shared_ptr<Context> ctx_;
};

// Saturation/contrast is an algorithm, never part of a format converter.
class SaturationContrast final : public Node {
 public:
  SaturationContrast(const std::string& id, graphflow::core::EventBus* bus, EditorColor color)
      : Node(id, bus), color_(color) {}

 protected:
  bool doInit() override {
    subscribe<MediaFrameEvent>([this](const auto& input) {
      auto e = input;
      e.image = std::make_shared<cv::Mat>(input.image->clone());
      auto& rgb = *e.image;
      for (int y = 0; y < rgb.rows; ++y) {
        for (int x = 0; x < rgb.cols; ++x) {
          auto& pixel = rgb.at<cv::Vec3b>(y, x);
          const double gray = .2126 * pixel[0] + .7152 * pixel[1] + .0722 * pixel[2];
          for (int c = 0; c < 3; ++c) {
            pixel[c] = cv::saturate_cast<uchar>(
                (gray + (pixel[c] - gray) * color_.saturation - 127.5) * color_.contrast + 127.5);
          }
        }
      }
      publish(e);
    });
    return true;
  }

 private:
  EditorColor color_;
};

class AudioWindow final : public Node {
 public:
  AudioWindow(const std::string& id, graphflow::core::EventBus* bus,
              std::function<void(Work&)> finish)
      : Node(id, bus), finish_(std::move(finish)) {}

 protected:
  bool doInit() override {
    subscribe<Layer>([this](const auto& layer) {
      if (!layer.work->error) {
        finish_(*layer.work);
      }
      publish(layer);
    });
    return true;
  }

 private:
  std::function<void(Work&)> finish_;
};

class AudioFade final : public Node {
 public:
  AudioFade(const std::string& id, graphflow::core::EventBus* bus, const EditorClip& clip,
            EditorRate rate)
      : Node(id, bus),
        in_(clip.fade_in),
        out_(clip.fade_out),
        duration_(clip.duration),
        fps_(double(rate.numerator) / rate.denominator) {}

 protected:
  bool doInit() override {
    subscribe<Layer>([this](const auto& layer) {
      auto& w = *layer.work;
      if (!w.error) {
        for (size_t i = 0; i < w.audio.size() / 2; ++i) {
          const double frame = w.output->frame - w.clip->position + double(i) * fps_ / 48000;
          double gain = 1;
          if (in_ > 0) {
            gain *= std::clamp(frame / in_, 0., 1.);
          }
          if (out_ > 0) {
            gain *= std::clamp((duration_ - frame) / out_, 0., 1.);
          }
          w.audio[i * 2] *= float(gain);
          w.audio[i * 2 + 1] *= float(gain);
        }
      }
      publish(layer);
    });
    return true;
  }

 private:
  int64_t in_, out_, duration_;
  double fps_;
};

class TextRender final : public Node {
 public:
  TextRender(const std::string& id, graphflow::core::EventBus* bus, EditorText text,
             std::shared_ptr<Context> context)
      : Node(id, bus), text_(std::move(text)), context_(std::move(context)) {}

 protected:
  bool doInit() override {
    subscribe<Layer>([this](const auto& layer) {
      auto& w = *layer.work;
      if (!w.error && !w.track->hidden) {
        // Cache belongs to this immutable text node; canvas changes invalidate it.
        auto cached = cached_.lock();
        if (!cached || cached->width != w.output->image.width ||
            cached->height != w.output->image.height) {
          auto image = std::make_shared<EditorImage>();
          image->width = w.output->image.width;
          image->height = w.output->image.height;
          image->rgba.resize(size_t(image->width) * image->height * 4, 0);
          std::string error;
          if (!render_text(*image, text_, error)) {
            w.error = editor_detail::failure(error);
          } else {
            cached = std::move(image);
            cached_ = cached;
            context_->retain_text(cached);
          }
        }
        if (!w.error) {
          w.image = std::move(cached);
        }
      }
      publish(layer);
    });
    return true;
  }

 private:
  EditorText text_;
  std::weak_ptr<EditorImage> cached_;
  std::shared_ptr<Context> context_;
};

// The join owns no cross-frame waiting state. Each transaction has two slots,
// so results from different generations or render requests cannot be combined.
class TransitionJoin final : public Node {
 public:
  using Node::Node;

 protected:
  bool doInit() override {
    subscribe<Layer>([this](const auto& layer) {
      auto& w = *layer.work;
      if (w.error || !w.transition) {
        return;
      }
      auto& joined = *w.transition;
      auto& slot = joined.inputs[w.transition_input];
      if (slot) {
        w.error = editor_detail::failure("transition: duplicate input");
        return;
      }
      slot = w.output;
      if (!joined.inputs[0] || !joined.inputs[1]) {
        return;
      }
      // Aliasing shared_ptrs keep both layer images alive through the algorithm.
      w.image = std::shared_ptr<const EditorImage>(joined.inputs[0], &joined.inputs[0]->image);
      w.next_image = std::shared_ptr<const EditorImage>(joined.inputs[1], &joined.inputs[1]->image);
      w.output = joined.output;
      w.audio.resize(w.output->audio.size());
      for (size_t i = 0; i < w.audio.size(); ++i) {
        w.audio[i] = float(joined.inputs[0]->audio[i] * (1 - joined.progress) +
                           joined.inputs[1]->audio[i] * joined.progress);
      }
      publish(layer);
    });
    return true;
  }
};

class TransitionCommit final : public Node {
 public:
  using Node::Node;

 protected:
  bool doInit() override {
    subscribe<Layer>([this](const auto& layer) {
      auto& w = *layer.work;
      if (!w.error) {
        w.output->image = *w.image;
        for (size_t i = 0; i < w.audio.size(); ++i) {
          w.output->audio[i] += w.audio[i];
        }
      }
      publish(layer);
    });
    return true;
  }
};
}  // namespace

class EffectNodes::Impl {
 public:
  struct Entry {
    std::string signature;
    std::shared_ptr<Node> node;
  };

  std::map<std::string, Entry> entries;
  Connections edges;
  std::shared_ptr<Context> context = std::make_shared<Context>();
  size_t reused = 0;

  template <class Factory>
  void add(const std::string& id, const Json& signature, const Impl* previous, Factory factory) {
    auto key = signature.dump();
    if (previous) {
      auto found = previous->entries.find(id);
      if (found != previous->entries.end() && found->second.signature == key) {
        entries.emplace(id, found->second);
        ++reused;
        return;
      }
    }
    auto node = factory();
    // Synchronous backend callbacks: only the current conversion is pending.
    node->setFailureHandler([ctx = context, id](auto) {
      for (const auto& entry : ctx->pending) {
        entry.second->error =
            editor_detail::failure(id + ": backend callback failed (details in GraphFlow log)");
      }
    });
    entries.emplace(id, Entry{std::move(key), std::move(node)});
  }

  template <class Algorithm>
  void algorithm(const std::string& id, const Json& params, graphflow::core::EventBus* bus,
                 const Impl* previous) {
    add(id, params, previous, [&] {
      graphflow::engine::NodeConfig config;
      config.nodeId = id;
      config.eventBus = bus;
      config.params = params;
      return std::make_shared<Algorithm>(config);
    });
  }
};

EffectNodes::EffectNodes() : impl_(std::make_unique<Impl>()) {}

EffectNodes::~EffectNodes() = default;

Result<std::unique_ptr<EffectNodes>> EffectNodes::prepare(const EditorProject& project,
                                                          graphflow::core::EventBus* bus,
                                                          std::function<void(Work&)> finish_audio,
                                                          const EffectNodes* previous) {
  try {
    auto plan = std::unique_ptr<EffectNodes>(new EffectNodes());
    auto& p = *plan->impl_;
    const auto* old = previous ? previous->impl_.get() : nullptr;
    if (old) {
      p.context = old->context;
    }
    std::vector<std::pair<std::string, std::string>> image_bypasses;
    auto image_branch = [&](const std::string& key) {
      p.add(key + ".rgba_to_rgb", {}, old,
            [&] { return std::make_shared<RgbaToRgb>(key + ".rgba_to_rgb", bus, p.context); });
      p.add(key + ".rgb_to_rgba", {}, old,
            [&] { return std::make_shared<RgbToRgba>(key + ".rgb_to_rgba", bus, p.context); });
      image_bypasses.emplace_back(key + ".rgba_to_rgb", key + ".rgb_to_rgba");
    };
    for (const auto& track : project.tracks) {
      for (const auto& clip : track.clips) {
        std::string previous_node = "editor.route";
        std::string condition = effect_route(clip.id, "pipeline");
        auto chain = [&](const std::string& first, const std::string& last) {
          p.edges.push_back({previous_node, first, "", condition});
          previous_node = last;
          condition.clear();
        };
        auto asset = std::find_if(project.assets.begin(), project.assets.end(),
                                  [&](const auto& a) { return a.id == clip.asset_id; });
        if (!clip.text.content.empty()) {
          const auto key = effect_route(clip.id, "text");
          const auto& t = clip.text;
          p.add(key + ".render",
                {t.content, t.font, t.size, t.alignment, t.color, t.outline_color, t.outline}, old,
                [&] { return std::make_shared<TextRender>(key + ".render", bus, t, p.context); });
          chain(key + ".render", key + ".render");
        }
        const auto& c = clip.color;
        if (clip.text.content.empty() && asset != project.assets.end() && asset->video &&
            (c.brightness != 0 || c.temperature != 0 || c.saturation != 1 || c.contrast != 1)) {
          const auto key = effect_route(clip.id, "color");
          image_branch(key);
          p.algorithm<nndeploy::algorithm::ColorGrade>(
              key + ".grade",
              {{"exposure", c.brightness},
               {"gains", {1 + c.temperature * .3, 1., 1 - c.temperature * .3}}},
              bus, old);
          p.add(key + ".saturation_contrast", {c.saturation, c.contrast}, old, [&] {
            return std::make_shared<SaturationContrast>(key + ".saturation_contrast", bus, c);
          });
          p.edges.push_back({key + ".rgba_to_rgb", key + ".grade",
                             graphflow::core::detail::prettyName<MediaFrameEvent>()});
          p.edges.push_back({key + ".grade", key + ".saturation_contrast",
                             graphflow::core::detail::prettyName<MediaFrameEvent>()});
          p.edges.push_back({key + ".saturation_contrast", key + ".rgb_to_rgba",
                             graphflow::core::detail::prettyName<MediaFrameEvent>()});
          chain(key + ".rgba_to_rgb", key + ".rgb_to_rgba");
        }
        if (clip.text.content.empty() && asset != project.assets.end() && asset->audio &&
            clip.speed != 1) {
          const auto key = effect_route(clip.id, "audio");
          p.add(key + ".float_to_audio", {}, old, [&] {
            return std::make_shared<FloatToAudio>(key + ".float_to_audio", bus, p.context);
          });
          p.algorithm<nndeploy::algorithm::TimeStretch>(key + ".time_stretch",
                                                        {{"rate", clip.speed}}, bus, old);
          p.add(key + ".audio_to_float", {}, old, [&] {
            return std::make_shared<AudioToFloat>(key + ".audio_to_float", bus, p.context);
          });
          p.add(key + ".window", {}, old,
                [&] { return std::make_shared<AudioWindow>(key + ".window", bus, finish_audio); });
          p.edges.push_back({key + ".float_to_audio", key + ".time_stretch",
                             graphflow::core::detail::prettyName<AudioEvent>()});
          p.edges.push_back({key + ".time_stretch", key + ".audio_to_float",
                             graphflow::core::detail::prettyName<AudioEvent>()});
          p.edges.push_back({key + ".float_to_audio", key + ".window", "", "bypass"});
          p.edges.push_back({key + ".audio_to_float", key + ".window"});
          chain(key + ".float_to_audio", key + ".window");
        }
        if (clip.text.content.empty() && asset != project.assets.end() && asset->audio &&
            (clip.fade_in > 0 || clip.fade_out > 0)) {
          const auto key = effect_route(clip.id, "fade") + ".audio_fade";
          p.add(key,
                {clip.fade_in, clip.fade_out, clip.duration, project.rate.numerator,
                 project.rate.denominator},
                old, [&] { return std::make_shared<AudioFade>(key, bus, clip, project.rate); });
          chain(key, key);
        }
        p.edges.push_back({previous_node, "editor.composite", "", condition});
        if (clip.transition.duration > 0) {
          const auto key = effect_route(clip.id, "transition");
          image_branch(key);
          p.add(key + ".join", {}, old,
                [&] { return std::make_shared<TransitionJoin>(key + ".join", bus); });
          p.add(key + ".commit", {}, old,
                [&] { return std::make_shared<TransitionCommit>(key + ".commit", bus); });
          p.algorithm<nndeploy::algorithm::Transition>(
              key + ".transition",
              {{"effect", clip.transition.effect}, {"params", {{"start", 0}, {"duration", 1}}}},
              bus, old);
          p.edges.push_back({"editor.result_route", key + ".join", "", key});
          p.edges.push_back({key + ".join", key + ".rgba_to_rgb"});
          p.edges.push_back({key + ".rgba_to_rgb", key + ".transition",
                             graphflow::core::detail::prettyName<MediaFrameEvent>()});
          p.edges.push_back({key + ".transition", key + ".rgb_to_rgba",
                             graphflow::core::detail::prettyName<MediaFrameEvent>()});
          p.edges.push_back({key + ".rgb_to_rgba", key + ".commit"});
          p.edges.push_back({key + ".commit", "editor.output"});
        }
      }
    }
    // Bypass skips both conversions and rejoins the next Layer-processing node.
    // Keep each conversion node's inputs homogeneous: EventBus has one handler
    // per edge topic, and GraphNode subscriptions enforce its declared type.
    for (const auto& bypass : image_bypasses) {
      auto next = std::find_if(p.edges.begin(), p.edges.end(), [&](const auto& edge) {
        return edge.from == bypass.second && edge.condition.empty();
      });
      if (next == p.edges.end()) {
        return Result<std::unique_ptr<EffectNodes>>::failure(
            failure("effect topology: missing image continuation"));
      }
      const auto target = next->to;
      p.edges.push_back({bypass.first, target, "", "bypass"});
    }
    for (auto& edge : p.edges) {
      if (edge.dataType.empty()) {
        edge.dataType = graphflow::core::detail::prettyName<Layer>();
      }
    }
    return Result<std::unique_ptr<EffectNodes>>::success(std::move(plan));
  } catch (const std::exception& e) {
    return Result<std::unique_ptr<EffectNodes>>::failure(
        failure(std::string("effect topology: ") + e.what()));
  }
}

void EffectNodes::append_to(Nodes& nodes, Connections& connections) const {
  for (const auto& entry : impl_->entries) {
    nodes.push_back(entry.second.node);
  }
  connections.insert(connections.end(), impl_->edges.begin(), impl_->edges.end());
}

size_t EffectNodes::reused_nodes() const {
  return impl_->reused;
}

void EffectNodes::finish_frame() {
  impl_->context->pending.clear();
}

bool render_text(EditorImage& image, const EditorText& text, std::string& error) noexcept {
  // The FFmpeg subtitles filter uses libass; both preview and export call this renderer.
  struct Resources {
    AVFilterGraph* graph = nullptr;
    AVFrame* frame = nullptr;
    std::filesystem::path path;

    ~Resources() {
      av_frame_free(&frame);
      avfilter_graph_free(&graph);
      std::error_code ec;
      if (!path.empty()) {
        std::filesystem::remove(path, ec);
      }
    }
  } resources;

  auto& graph = resources.graph;
  auto& frame = resources.frame;
  auto& path = resources.path;
  try {
    static std::atomic<uint64_t> serial{0};
    path =
        std::filesystem::temp_directory_path() /
        ("freecut-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial) + ".ass");
    auto ass_color = [](uint32_t rgb) {
      std::ostringstream out;
      out << "&H00" << std::hex << std::setfill('0') << std::setw(2) << (rgb & 255) << std::setw(2)
          << ((rgb >> 8) & 255) << std::setw(2) << ((rgb >> 16) & 255);
      return out.str();
    };
    std::string content;
    for (char c : text.content) {
      if (c == '\n') {
        content += "\\N";
      } else if (c == '\r') {
        continue;
      } else if (c == '{' || c == '}' || c == '\\') {
        content += ' ';
      } else {
        content += c;
      }
    }
    std::string font = text.font;
    for (auto& c : font) {
      if (c == ',' || c == '\n' || c == '\r') {
        c = ' ';
      }
    }
    std::ofstream file(path, std::ios::binary);
    file << "[Script Info]\nScriptType: v4.00+\nPlayResX: " << image.width
         << "\nPlayResY: " << image.height
         << "\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
            "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, "
            "Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, "
            "Encoding\nStyle: Default,"
         << font << ',' << text.size << ',' << ass_color(text.color) << ",&H00FFFFFF,"
         << ass_color(text.outline_color) << ",&H00000000,0,0,0,0,100,100,0,0,1," << text.outline
         << ",0," << text.alignment
         << ",30,30,30,1\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, "
            "MarginV, Effect, Text\nDialogue: 0,0:00:00.00,9:59:59.00,Default,,0,0,0,,"
         << content << '\n';
    file.close();
    if (!file) {
      error = "cannot write subtitle render input";
      return false;
    }
    graph = avfilter_graph_alloc();
    if (!graph) {
      error = "subtitle graph allocation failed";
      return false;
    }
    AVFilterContext *source = nullptr, *sink = nullptr;
    std::string args =
        "video_size=" + std::to_string(image.width) + "x" + std::to_string(image.height) +
        ":pix_fmt=" + std::to_string(AV_PIX_FMT_RGBA) + ":time_base=1/30:pixel_aspect=1/1";
    auto check = [&](int rc) {
      if (rc < 0) {
        error = "libass filter failed: " + std::to_string(rc);
        return false;
      }
      return true;
    };
    if (!check(avfilter_graph_create_filter(&source, avfilter_get_by_name("buffer"), "in",
                                            args.c_str(), nullptr, graph))) {
      return false;
    }
    const auto* filter = avfilter_get_by_name("subtitles");
    if (!filter) {
      error = "FFmpeg requires the libass subtitles filter";
      return false;
    }
    auto* subtitles = avfilter_graph_alloc_filter(graph, filter, "text");
    if (!subtitles) {
      error = "subtitle filter allocation failed";
      return false;
    }
    if (!check(
            av_opt_set(subtitles, "filename", path.u8string().c_str(), AV_OPT_SEARCH_CHILDREN))) {
      return false;
    }
    if (!check(av_opt_set(subtitles, "alpha", "1", AV_OPT_SEARCH_CHILDREN))) {
      return false;
    }
    if (!check(avfilter_init_str(subtitles, nullptr))) {
      return false;
    }
    if (!check(avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out",
                                            nullptr, nullptr, graph))) {
      return false;
    }
    if (!check(avfilter_link(source, 0, subtitles, 0))) {
      return false;
    }
    if (!check(avfilter_link(subtitles, 0, sink, 0))) {
      return false;
    }
    if (!check(avfilter_graph_config(graph, nullptr))) {
      return false;
    }
    frame = av_frame_alloc();
    if (!frame) {
      error = "subtitle frame allocation failed";
      return false;
    }
    frame->width = image.width;
    frame->height = image.height;
    frame->format = AV_PIX_FMT_RGBA;
    frame->pts = 0;
    if (!check(av_frame_get_buffer(frame, 0))) {
      return false;
    }
    for (int y = 0; y < image.height; ++y) {
      memcpy(frame->data[0] + y * frame->linesize[0],
             image.rgba.data() + size_t(y) * image.width * 4, image.width * 4);
    }
    if (!check(av_buffersrc_add_frame(source, frame))) {
      return false;
    }
    if (!check(av_buffersink_get_frame(sink, frame))) {
      return false;
    }
    if (frame->format != AV_PIX_FMT_RGBA) {
      error = "unexpected subtitle pixel format";
      return false;
    }
    for (int y = 0; y < image.height; ++y) {
      memcpy(image.rgba.data() + size_t(y) * image.width * 4,
             frame->data[0] + y * frame->linesize[0], image.width * 4);
    }
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}
}  // namespace livekit::sdk::editor_detail
