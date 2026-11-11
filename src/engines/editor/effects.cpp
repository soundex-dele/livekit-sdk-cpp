#include "effects.hpp"
#include <nndeploy/algorithm/algorithms.h>
#include <opencv2/imgproc.hpp>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <atomic>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
}

namespace livekit::sdk::editor_detail {
namespace {
using namespace nndeploy::flow;

template <class Event>
class Input final : public graphflow::core::GraphNode {
 public:
  Input(graphflow::core::EventBus* bus) : GraphNode("input", bus) {}

  void send(const Event& e) {
    publish(e);
  }
};

template <class Event>
class Output final : public graphflow::core::GraphNode {
 public:
  Output(graphflow::core::EventBus* bus) : GraphNode("output", bus) {}

  std::optional<Event> result;

 protected:
  bool doInit() override {
    subscribe<Event>([this](const Event& e) { result = e; });
    return true;
  }
};

template <class Node, class Event>
class EffectGraph {
 public:
  explicit EffectGraph(const Json& params) {
    source_ = std::make_shared<Input<Event>>(engine_.eventBus());
    sink_ = std::make_shared<Output<Event>>(engine_.eventBus());
    graphflow::engine::NodeConfig config;
    config.nodeId = "effect";
    config.eventBus = engine_.eventBus();
    config.params = params;
    auto node = std::make_shared<Node>(config);
    if (!engine_.loadFromNodes({source_, node, sink_},
                               {{"input", "effect", "", ""}, {"effect", "output", "", ""}}) ||
        !engine_.init() || !engine_.start()) {
      engine_.release();
      return;
    }
    ready_ = true;
  }

  ~EffectGraph() {
    engine_.release();
  }

  std::optional<Event> run(const Event& input) {
    if (!ready_) {
      return std::nullopt;
    }
    // A failed invocation must never return an earlier successful result.
    sink_->result.reset();
    source_->send(input);
    if (!sink_->result) {
      return std::nullopt;
    }
    Event result = std::move(*sink_->result);
    sink_->result.reset();
    return result;
  }

 private:
  bool ready_ = false;
  graphflow::engine::GraphEngine engine_{1};
  std::shared_ptr<Input<Event>> source_;
  std::shared_ptr<Output<Event>> sink_;
};

template <class Node, class Event>
class EffectCache {
 public:
  std::optional<Event> run(Event input, const Json& params, const std::string& session,
                           uint64_t frame) {
    const auto key = params.dump();
    auto found = graphs_.find(key);
    if (found == graphs_.end()) {
      if (graphs_.size() >= 8) {
        graphs_.erase(graphs_.begin());
      }
      found = graphs_.emplace(key, std::make_unique<EffectGraph<Node, Event>>(params)).first;
    }
    input.session_id = session;
    input.frame_id = frame;
    return found->second->run(input);
  }

 private:
  std::map<std::string, std::unique_ptr<EffectGraph<Node, Event>>> graphs_;
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
}  // namespace

class EffectSession::Impl {
 public:
  Impl() {
    static std::atomic<uint64_t> next{0};
    session = "freecut-" + std::to_string(++next);
  }

  std::string session;
  uint64_t sequence = 0;
  EffectCache<nndeploy::algorithm::ColorGrade, MediaFrameEvent> color;
  EffectCache<nndeploy::algorithm::Transition, MediaFrameEvent> transitions;
  EffectCache<nndeploy::algorithm::TimeStretch, AudioEvent> audio;
};

EffectSession::EffectSession() : impl_(std::make_unique<Impl>()) {}

EffectSession::~EffectSession() = default;

bool EffectSession::grade(EditorImage& image, const EditorColor& color,
                          std::string& error) noexcept {
  try {
    auto e =
        impl_->color.run(event(image),
                         {{"exposure", color.brightness},
                          {"gains", {1 + color.temperature * .3, 1., 1 - color.temperature * .3}}},
                         impl_->session, ++impl_->sequence);
    if (!e) {
      error = "color graph failed to produce output";
      return false;
    }
    auto& rgb = *e->image;
    for (int y = 0; y < rgb.rows; ++y) {
      for (int x = 0; x < rgb.cols; ++x) {
        auto& pixel = rgb.at<cv::Vec3b>(y, x);
        const double gray = .2126 * pixel[0] + .7152 * pixel[1] + .0722 * pixel[2];
        for (int c = 0; c < 3; ++c) {
          pixel[c] = cv::saturate_cast<uchar>(
              (gray + (pixel[c] - gray) * color.saturation - 127.5) * color.contrast + 127.5);
        }
      }
    }
    copy_rgb(image, rgb);
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

bool EffectSession::transition(EditorImage& image, const EditorImage& next,
                               const std::string& effect, double progress,
                               std::string& error) noexcept {
  try {
    auto e = event(image);
    e.reference = event(next).image;
    e.timestamp = progress;
    auto result =
        impl_->transitions.run(e, {{"effect", effect}, {"params", {{"start", 0}, {"duration", 1}}}},
                               impl_->session, ++impl_->sequence);
    if (!result) {
      error = "transition graph failed to produce output";
      return false;
    }
    copy_rgb(image, *result->image);
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

bool EffectSession::stretch_audio(std::vector<float>& audio, double rate,
                                  std::string& error) noexcept {
  try {
    AudioEvent e;
    e.session_id = "freecut";
    e.sample_rate = 48000;
    e.samples.shape = {audio.size() / 2, 2};
    e.samples.data.assign(audio.begin(), audio.end());
    auto result = impl_->audio.run(e, {{"rate", rate}}, impl_->session, ++impl_->sequence);
    if (!result) {
      error = "audio graph failed to produce output";
      return false;
    }
    audio.resize(result->samples.data.size());
    std::transform(result->samples.data.begin(), result->samples.data.end(), audio.begin(),
                   [](double value) { return float(value); });
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
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
