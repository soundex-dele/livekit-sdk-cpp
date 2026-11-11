#include "editor_internal.hpp"
#include "engine/graph_engine.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <cmath>
#include <map>

namespace livekit::sdk::editor_detail {
namespace {
constexpr AVRational kUs{1, 1000000};
constexpr AVRational kSamples{1, 48000};

Failure ff_error(int code, const std::string& op) {
  char text[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(code, text, sizeof(text));
  return failure(op + ": " + text);
}

// Each decoder owns its demuxer. Video and audio can seek independently without
// retaining unrelated packets. All calls occur on the graph's owner worker.
class Decoder {
 public:
  explicit Decoder(std::atomic<bool>& cancel) : cancel_(cancel) {}

  ~Decoder() {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_);
    avformat_close_input(&format_);
    sws_freeContext(scaler_);
    swr_free(&resampler_);
  }

  Result<void> open(const std::string& path, bool video) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), ec)) {
      return Result<void>::failure(failure("open: local source missing: " + path));
    }
    format_ = avformat_alloc_context();
    if (!format_) {
      return Result<void>::failure(failure("decode: allocation failed"));
    }
    format_->interrupt_callback = {
        [](void* opaque) { return static_cast<Decoder*>(opaque)->cancel_.load() ? 1 : 0; }, this};
    int rc = avformat_open_input(&format_, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
      return Result<void>::failure(ff_error(rc, "open " + path));
    }
    rc = avformat_find_stream_info(format_, nullptr);
    if (rc < 0) {
      return Result<void>::failure(ff_error(rc, "probe " + path));
    }
    stream_ = av_find_best_stream(format_, video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO, -1, -1,
                                  nullptr, 0);
    if (stream_ < 0) {
      return Result<void>::failure(ff_error(stream_, "find stream " + path));
    }
    auto* params = format_->streams[stream_]->codecpar;
    const auto* decoder = avcodec_find_decoder(params->codec_id);
    if (!decoder) {
      return Result<void>::failure(failure("decode: unsupported codec " + path));
    }
    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_) {
      return Result<void>::failure(failure("decode: allocation failed"));
    }
    rc = avcodec_parameters_to_context(codec_, params);
    if (rc < 0) {
      return Result<void>::failure(ff_error(rc, "decode parameters"));
    }
    codec_->thread_count = 2;
    codec_->max_pixels = 4096LL * 4096;
    codec_->max_samples = 192000;
    rc = avcodec_open2(codec_, decoder, nullptr);
    if (rc < 0) {
      return Result<void>::failure(ff_error(rc, "decode codec open"));
    }
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!frame_ || !packet_) {
      return Result<void>::failure(failure("decode: allocation failed"));
    }
    origin_ = format_->start_time == AV_NOPTS_VALUE ? 0 : format_->start_time;
    return Result<void>::success();
  }

  Result<bool> next() {
    for (;;) {
      if (cancel_) {
        return Result<bool>::failure(failure("decode: cancelled", ErrorCode::cancelled));
      }
      int rc = avcodec_receive_frame(codec_, frame_);
      if (rc == 0) {
        return Result<bool>::success(true);
      }
      if (rc == AVERROR_EOF) {
        return Result<bool>::success(false);
      }
      if (rc != AVERROR(EAGAIN)) {
        return Result<bool>::failure(ff_error(rc, "decode receive"));
      }
      if (eof_) {
        return Result<bool>::success(false);
      }
      do {
        av_packet_unref(packet_);
        rc = av_read_frame(format_, packet_);
      } while (rc >= 0 && packet_->stream_index != stream_ && !cancel_);
      if (rc == AVERROR_EOF) {
        eof_ = true;
        rc = avcodec_send_packet(codec_, nullptr);
      } else if (rc >= 0) {
        rc = avcodec_send_packet(codec_, packet_);
      }
      av_packet_unref(packet_);
      if (rc < 0) {
        return Result<bool>::failure(ff_error(rc, "decode read/send"));
      }
    }
  }

  Result<void> seek(int64_t us) {
    const auto ts = av_rescale_q(std::max<int64_t>(0, us - 200000) + origin_, kUs,
                                 format_->streams[stream_]->time_base);
    int rc = av_seek_frame(format_, stream_, ts, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
      return Result<void>::failure(ff_error(rc, "decode seek"));
    }
    avcodec_flush_buffers(codec_);
    eof_ = false;
    previous_.reset();
    upcoming_.reset();
    pcm_.clear();
    swr_free(&resampler_);
    return Result<void>::success();
  }

  int64_t timestamp() const {
    const auto pts = frame_->best_effort_timestamp;
    return pts == AV_NOPTS_VALUE
               ? 0
               : av_rescale_q(pts, format_->streams[stream_]->time_base, kUs) - origin_;
  }

  Result<std::shared_ptr<const EditorImage>> video(int64_t us, bool still) {
    if (!still && ((last_request_ < 0 && us > 1000000) ||
                   (last_request_ >= 0 && (us < last_request_ || us - last_request_ > 1000000)))) {
      auto result = seek(us);
      if (!result) {
        return Result<std::shared_ptr<const EditorImage>>::failure(result.failure_record());
      }
    }
    last_request_ = us;
    for (;;) {
      if (upcoming_) {
        if (!still && upcoming_us_ > us) {
          if (previous_) {
            break;
          }
          return Result<std::shared_ptr<const EditorImage>>::success({});
        }
        previous_ = upcoming_;
        previous_us_ = upcoming_us_;
        upcoming_.reset();
        if (still || previous_us_ > us) {
          break;
        }
      }
      if (still && previous_) {
        break;
      }
      auto decoded = next();
      if (!decoded) {
        return Result<std::shared_ptr<const EditorImage>>::failure(decoded.failure_record());
      }
      if (!*decoded) {
        break;
      }
      if (frame_->width <= 0 || frame_->height <= 0 || frame_->width > 4096 ||
          frame_->height > 4096) {
        return Result<std::shared_ptr<const EditorImage>>::failure(
            failure("video: dimensions exceed 4096"));
      }
      auto image = std::make_shared<EditorImage>();
      image->width = frame_->width;
      image->height = frame_->height;
      image->rgba.resize(size_t(image->width) * image->height * 4);
      scaler_ = sws_getCachedContext(
          scaler_, frame_->width, frame_->height, static_cast<AVPixelFormat>(frame_->format),
          image->width, image->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!scaler_) {
        return Result<std::shared_ptr<const EditorImage>>::failure(failure("video: scaler failed"));
      }
      const int cs = frame_->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
      sws_setColorspaceDetails(scaler_, sws_getCoefficients(cs),
                               frame_->color_range == AVCOL_RANGE_JPEG, sws_getCoefficients(cs), 1,
                               0, 1 << 16, 1 << 16);
      uint8_t* data[4]{image->rgba.data()};
      int stride[4]{image->width * 4};
      if (sws_scale(scaler_, frame_->data, frame_->linesize, 0, frame_->height, data, stride) < 0) {
        return Result<std::shared_ptr<const EditorImage>>::failure(
            failure("video: conversion failed"));
      }
      upcoming_ = std::move(image);
      upcoming_us_ = timestamp();
    }
    if (!previous_) {
      return Result<std::shared_ptr<const EditorImage>>::failure(
          failure("video: no decoded frame"));
    }
    return Result<std::shared_ptr<const EditorImage>>::success(previous_);
  }

  Result<std::vector<float>> audio(int64_t sample, int count) {
    if ((last_request_ < 0 && sample > 48000) ||
        (last_request_ >= 0 && (sample < last_request_ || sample - last_request_ > 48000))) {
      auto result = seek(av_rescale_q(sample, kSamples, kUs));
      if (!result) {
        return Result<std::vector<float>>::failure(result.failure_record());
      }
    }
    last_request_ = sample;
    std::vector<float> out(size_t(count) * 2, 0);
    const int64_t end = sample + count;
    for (;;) {
      if (!pcm_.empty()) {
        const int64_t pcm_end = pcm_start_ + pcm_.size() / 2;
        const auto start = std::max(sample, pcm_start_), stop = std::min(end, pcm_end);
        if (stop > start) {
          std::copy(pcm_.begin() + (start - pcm_start_) * 2, pcm_.begin() + (stop - pcm_start_) * 2,
                    out.begin() + (start - sample) * 2);
        }
        if (pcm_end >= end) {
          break;
        }
        pcm_.clear();
      }
      auto decoded = next();
      if (!decoded) {
        return Result<std::vector<float>>::failure(decoded.failure_record());
      }
      if (!*decoded) {
        break;
      }
      if (frame_->sample_rate <= 0 || frame_->nb_samples > 192000) {
        return Result<std::vector<float>>::failure(failure("audio: invalid frame size/rate"));
      }
      if (!resampler_) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        int rc = swr_alloc_set_opts2(
            &resampler_, &stereo, AV_SAMPLE_FMT_FLT, 48000, &frame_->ch_layout,
            static_cast<AVSampleFormat>(frame_->format), frame_->sample_rate, 0, nullptr);
        if (rc < 0 || !resampler_ || (rc = swr_init(resampler_)) < 0) {
          return Result<std::vector<float>>::failure(ff_error(rc, "audio resampler"));
        }
        next_pcm_ = av_rescale_q(timestamp(), kUs, kSamples);
      }
      const auto frame_start = av_rescale_q(timestamp(), kUs, kSamples);
      const auto delay = swr_get_delay(resampler_, frame_->sample_rate);
      // Preserve real gaps while avoiding per-packet rounding discontinuities.
      if (std::abs(frame_start - next_pcm_ - av_rescale(delay, 48000, frame_->sample_rate)) >
          2400) {
        swr_close(resampler_);
        swr_init(resampler_);
        next_pcm_ = frame_start;
      }
      const int capacity = swr_get_out_samples(resampler_, frame_->nb_samples);
      if (capacity < 0 || capacity > 384000) {
        return Result<std::vector<float>>::failure(failure("audio: resample buffer limit"));
      }
      pcm_.resize(size_t(capacity) * 2);
      uint8_t* output[]{reinterpret_cast<uint8_t*>(pcm_.data())};
      const int n =
          swr_convert(resampler_, output, capacity,
                      const_cast<const uint8_t**>(frame_->extended_data), frame_->nb_samples);
      if (n < 0) {
        return Result<std::vector<float>>::failure(ff_error(n, "audio convert"));
      }
      pcm_.resize(size_t(n) * 2);
      pcm_start_ = next_pcm_;
      next_pcm_ += n;
    }
    return Result<std::vector<float>>::success(std::move(out));
  }

 private:
  std::atomic<bool>& cancel_;
  AVFormatContext* format_ = nullptr;
  AVCodecContext* codec_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  SwsContext* scaler_ = nullptr;
  SwrContext* resampler_ = nullptr;
  int stream_ = -1;
  int64_t origin_ = 0, last_request_ = -1, previous_us_ = 0, upcoming_us_ = 0;
  int64_t pcm_start_ = 0, next_pcm_ = 0;
  bool eof_ = false;
  std::shared_ptr<const EditorImage> previous_, upcoming_;
  std::vector<float> pcm_;
};

void composite(EditorImage& out, const EditorImage& input, const EditorClip& c) {
  const double factor =
      std::min(double(out.width) / input.width, double(out.height) / input.height) * c.scale;
  const int w = std::max(1, int(input.width * factor)), h = std::max(1, int(input.height * factor));
  const int left = (out.width - w) / 2 + int(c.x * out.width);
  const int top = (out.height - h) / 2 + int(c.y * out.height);
  for (int y = std::max(0, top); y < std::min(out.height, top + h); ++y) {
    const int sy = std::min(input.height - 1, int((y - top) / factor));
    for (int x = std::max(0, left); x < std::min(out.width, left + w); ++x) {
      const int sx = std::min(input.width - 1, int((x - left) / factor));
      const auto* src = input.rgba.data() + (size_t(sy) * input.width + sx) * 4;
      auto* dst = out.rgba.data() + (size_t(y) * out.width + x) * 4;
      const double alpha = src[3] / 255. * c.opacity;
      for (int k = 0; k < 3; ++k) {
        dst[k] = uint8_t(std::lround(src[k] * alpha + dst[k] * (1 - alpha)));
      }
    }
  }
}

struct Work {
  std::shared_ptr<EditorFrame> output;
  const EditorTrack* track = nullptr;
  const EditorClip* clip = nullptr;
  const EditorAsset* asset = nullptr;
  int64_t source_frame = 0;
  std::shared_ptr<const EditorImage> image;
  std::vector<float> audio;
  Failure error;
  bool delivered = false;
};

struct Layer : graphflow::core::EventT<Layer> {
  std::shared_ptr<Work> work;
};

class Stage final : public graphflow::core::GraphNode {
 public:
  Stage(std::string name, graphflow::core::EventBus* bus, std::function<void(Work&)> fn)
      : GraphNode(name, bus), fn_(std::move(fn)) {}

  void send(const std::shared_ptr<Work>& work) {
    Layer layer;
    layer.work = work;
    publish(layer);
  }

 protected:
  bool doInit() override {
    subscribe<Layer>([this](Layer& layer) {
      if (!layer.work->error) {
        fn_(*layer.work);
      }
      publish(layer);
    });
    return true;
  }

 private:
  std::function<void(Work&)> fn_;
};
}  // namespace

Result<EditorProbe> probe(const std::string& path, std::atomic<bool>& cancel) {
  std::error_code ec;
  auto local = std::filesystem::absolute(std::filesystem::u8path(path), ec);
  if (ec || !std::filesystem::is_regular_file(local, ec)) {
    return Result<EditorProbe>::failure(failure("probe: local media file is missing: " + path));
  }
  AVFormatContext* format = avformat_alloc_context();
  if (!format) {
    return Result<EditorProbe>::failure(failure("probe: allocation failed"));
  }
  format->interrupt_callback = {
      [](void* p) { return static_cast<std::atomic<bool>*>(p)->load() ? 1 : 0; }, &cancel};
  int rc = avformat_open_input(&format, local.u8string().c_str(), nullptr, nullptr);
  if (rc >= 0) {
    rc = avformat_find_stream_info(format, nullptr);
  }
  if (rc < 0) {
    avformat_close_input(&format);
    return Result<EditorProbe>::failure(ff_error(rc, "probe " + path));
  }
  EditorProbe result;
  auto& a = result.asset;
  a.id = std::to_string(next_failure_id());
  a.name = local.filename().u8string();
  a.path = local.u8string();
  a.duration_us = format->duration > 0 ? format->duration : 0;
  const int vi = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  a.audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0;
  a.video = vi >= 0;
  if (a.video) {
    const auto* par = format->streams[vi]->codecpar;
    a.width = par->width;
    a.height = par->height;
    const std::string name = format->iformat->name;
    a.image = !a.audio && (name.find("image2") != std::string::npos ||
                           name.find("_pipe") != std::string::npos || name == "webp_pipe");
  }
  avformat_close_input(&format);
  if ((!a.video && !a.audio) || (!a.image && a.duration_us <= 0)) {
    return Result<EditorProbe>::failure(failure("probe: no finite playable media stream"));
  }
  if (a.video) {
    Decoder decoder(cancel);
    auto opened = decoder.open(a.path, true);
    if (!opened) {
      return Result<EditorProbe>::failure(opened.failure_record());
    }
    auto frame = decoder.video(0, true);
    if (!frame) {
      return Result<EditorProbe>::failure(frame.failure_record());
    }
    result.thumbnail.width = 160;
    result.thumbnail.height = std::max(1, (*frame)->height * 160 / (*frame)->width);
    result.thumbnail.rgba.resize(size_t(160) * result.thumbnail.height * 4, 255);
    EditorClip transform;
    composite(result.thumbnail, **frame, transform);
  }
  return Result<EditorProbe>::success(std::move(result));
}

class RenderGraph::Impl {
 public:
  Impl(const EditorProject& p, std::atomic<bool>& cancel) : project(p), cancel(cancel) {
    clock = std::make_shared<Stage>("editor.timeline", engine.eventBus(), [](Work&) {});
    decode =
        std::make_shared<Stage>("editor.decode", engine.eventBus(), [this](Work& w) { read(w); });
    compose = std::make_shared<Stage>("editor.composite", engine.eventBus(), [](Work& w) {
      if (w.image) {
        composite(w.output->image, *w.image, *w.clip);
      }
    });
    mix = std::make_shared<Stage>("editor.mix", engine.eventBus(), [](Work& w) {
      for (size_t i = 0; i < w.audio.size(); ++i) {
        w.output->audio[i] += w.audio[i] * float(w.clip->volume);
      }
    });
    output = std::make_shared<Stage>("editor.output", engine.eventBus(),
                                     [](Work& w) { w.delivered = true; });
    ready = engine.loadFromNodes({output, mix, compose, decode, clock},
                                 {{"editor.timeline", "editor.decode", "", ""},
                                  {"editor.decode", "editor.composite", "", ""},
                                  {"editor.composite", "editor.mix", "", ""},
                                  {"editor.mix", "editor.output", "", ""}}) &&
            engine.init() && engine.start();
  }

  ~Impl() {
    engine.release();
  }

  Result<Decoder*> get(const EditorAsset& asset, bool video) {
    const auto key = asset.id + (video ? "/video" : "/audio");
    auto it = decoders.find(key);
    if (it == decoders.end()) {
      const size_t weight = video ? size_t(asset.width) * asset.height * 8 : 4 * 1024 * 1024;
      auto cached_bytes = [&] {
        size_t bytes = 0;
        for (const auto& entry : decoders) {
          bytes += entry.second.weight;
        }
        return bytes;
      };
      while (!decoders.empty() &&
             (decoders.size() >= 8 || cached_bytes() + weight > 256 * 1024 * 1024)) {
        auto oldest = std::min_element(
            decoders.begin(), decoders.end(),
            [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
        decoders.erase(oldest);
      }
      auto decoder = std::make_unique<Decoder>(cancel);
      auto opened = decoder->open(asset.path, video);
      if (!opened) {
        return Result<Decoder*>::failure(opened.failure_record());
      }
      it = decoders.emplace(key, Entry{std::move(decoder), 0, weight}).first;
    }
    it->second.used = ++access;
    return Result<Decoder*>::success(it->second.decoder.get());
  }

  void read(Work& w) {
    if (cancel) {
      w.error = failure("render: cancelled", ErrorCode::cancelled);
      return;
    }
    if (w.asset->video && !w.track->audio_only && !w.track->hidden) {
      auto decoder = get(*w.asset, true);
      if (!decoder) {
        w.error = decoder.failure_record();
        return;
      }
      auto image = (*decoder)->video(editor_time_us(w.source_frame, project.rate), w.asset->image);
      if (!image) {
        w.error = image.failure_record();
        return;
      }
      w.image = *image;
    }
    if (w.asset->audio && !w.track->muted && w.clip->volume > 0) {
      auto decoder = get(*w.asset, false);
      if (!decoder) {
        w.error = decoder.failure_record();
        return;
      }
      const int64_t sample =
          w.source_frame * int64_t(project.rate.denominator) * 48000 / project.rate.numerator;
      auto audio = (*decoder)->audio(sample, int(w.output->audio.size() / 2));
      if (!audio) {
        w.error = audio.failure_record();
        return;
      }
      w.audio = std::move(*audio);
    }
  }

  Result<std::shared_ptr<EditorFrame>> render(int64_t frame, uint64_t generation) {
    if (!ready) {
      return Result<std::shared_ptr<EditorFrame>>::failure(
          failure("render: graph initialization failed"));
    }
    auto out = std::make_shared<EditorFrame>();
    out->frame = frame;
    out->generation = generation;
    out->image.width = project.width;
    out->image.height = project.height;
    out->image.rgba.resize(size_t(project.width) * project.height * 4, 0);
    for (size_t i = 3; i < out->image.rgba.size(); i += 4) {
      out->image.rgba[i] = 255;
    }
    const auto first_sample =
        frame * int64_t(project.rate.denominator) * 48000 / project.rate.numerator;
    const auto end_sample =
        (frame + 1) * int64_t(project.rate.denominator) * 48000 / project.rate.numerator;
    out->audio.resize(size_t(end_sample - first_sample) * 2, 0);
    for (const auto& track : project.tracks) {
      for (const auto& clip : track.clips) {
        if (frame < clip.position || frame >= clip.position + clip.duration) {
          continue;
        }
        auto asset = std::find_if(project.assets.begin(), project.assets.end(),
                                  [&](const auto& a) { return a.id == clip.asset_id; });
        if (asset == project.assets.end()) {
          return Result<std::shared_ptr<EditorFrame>>::failure(
              failure("render: missing asset reference"));
        }
        auto work = std::make_shared<Work>();
        work->output = out;
        work->track = &track;
        work->clip = &clip;
        work->asset = &*asset;
        work->source_frame = clip.source_in + frame - clip.position;
        clock->send(work);
        if (work->error) {
          return Result<std::shared_ptr<EditorFrame>>::failure(
              with_context(work->error, {"editor.render", track.id, clip.id, generation}));
        }
        if (!work->delivered) {
          return Result<std::shared_ptr<EditorFrame>>::failure(
              failure("render: disconnected graph"));
        }
      }
    }
    for (auto& v : out->audio) {
      v = std::isfinite(v) ? std::clamp(v, -1.f, 1.f) : 0.f;
    }
    return Result<std::shared_ptr<EditorFrame>>::success(std::move(out));
  }

  struct Entry {
    std::unique_ptr<Decoder> decoder;
    uint64_t used;
    size_t weight;
  };

  EditorProject project;
  std::atomic<bool>& cancel;
  graphflow::engine::GraphEngine engine{1};
  std::shared_ptr<Stage> clock, decode, compose, mix, output;
  std::map<std::string, Entry> decoders;
  uint64_t access = 0;
  bool ready = false;
};

RenderGraph::RenderGraph(const EditorProject& p, std::atomic<bool>& cancel)
    : impl_(std::make_unique<Impl>(p, cancel)) {}

RenderGraph::~RenderGraph() = default;

Result<std::shared_ptr<EditorFrame>> RenderGraph::render(int64_t frame, uint64_t generation) {
  return impl_->render(frame, generation);
}
}  // namespace livekit::sdk::editor_detail
