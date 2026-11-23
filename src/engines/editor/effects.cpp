#include "effects.hpp"
#include <atomic>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
}

namespace livekit::sdk::editor_detail {
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
  std::error_code ec;
  const auto temp = std::filesystem::temp_directory_path(ec);
  if (ec) {
    error = "subtitle temp directory: " + ec.message();
    return false;
  }
  static std::atomic<uint64_t> serial{0};
  path = temp /
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
  std::string args = "video_size=" + std::to_string(image.width) + "x" +
                     std::to_string(image.height) + ":pix_fmt=" + std::to_string(AV_PIX_FMT_RGBA) +
                     ":time_base=1/30:pixel_aspect=1/1";
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
  if (!check(av_opt_set(subtitles, "filename", path.u8string().c_str(), AV_OPT_SEARCH_CHILDREN))) {
    return false;
  }
  if (!check(av_opt_set(subtitles, "alpha", "1", AV_OPT_SEARCH_CHILDREN))) {
    return false;
  }
  if (!check(avfilter_init_str(subtitles, nullptr))) {
    return false;
  }
  if (!check(avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr,
                                          nullptr, graph))) {
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
    memcpy(frame->data[0] + y * frame->linesize[0], image.rgba.data() + size_t(y) * image.width * 4,
           image.width * 4);
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
    memcpy(image.rgba.data() + size_t(y) * image.width * 4, frame->data[0] + y * frame->linesize[0],
           image.width * 4);
  }
  return true;
}
}  // namespace livekit::sdk::editor_detail
