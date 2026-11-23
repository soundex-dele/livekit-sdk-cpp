#include "livekit/compat/span.hpp"
#include "internal.hpp"
#include <algorithm>
#include <chrono>
#include <optional>
#include <limits>
#include "support/utf8.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace livekit::sdk::studio_detail {
static Result<void> check(int e) {
  if (e < 0) {
    char s[256]{};
    av_strerror(e, s, sizeof(s));
    return failed(std::string("录制失败: ") + s, ErrorCode::processing_failed, e);
  }
  return Result<void>::success();
}

Result<std::string> path8(const std::filesystem::path& path) {
  const auto& native = path.native();
  if (native.empty() ||
      native.find(std::filesystem::path::value_type{}) !=
          std::filesystem::path::string_type::npos ||
      native.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    return failed("Invalid recording path", ErrorCode::invalid_argument);
#ifdef _WIN32
  const int length =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, native.data(),
                          static_cast<int>(native.size()), nullptr, 0, nullptr, nullptr);
  if (!length)
    return failed("Invalid recording path encoding", ErrorCode::invalid_argument,
                  int(GetLastError()));
  std::string text(size_t(length), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, native.data(),
                          static_cast<int>(native.size()), text.data(), length, nullptr,
                          nullptr) != length)
    return failed("Cannot encode recording path", ErrorCode::invalid_argument, int(GetLastError()));
  return Result<std::string>::success(std::move(text));
#else
  if (!detail::valid_utf8(native))
    return failed("Invalid recording path encoding", ErrorCode::invalid_argument);
  return Result<std::string>::success(native);
#endif
}

// On Windows the file protocol normally truncates an existing path. Claim each
// staging name before FFmpeg opens it, and pin the directory entry across that open.
static Result<void> openStage(AVIOContext** io, const std::filesystem::path& path,
                              const std::string& utf8) {
#ifdef _WIN32
  HANDLE claim = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (claim == INVALID_HANDLE_VALUE) {
    const auto code = GetLastError();
    return failed("Cannot create new recording file: " + utf8, ErrorCode::processing_failed, code);
  }
  const int result = avio_open(io, utf8.c_str(), AVIO_FLAG_WRITE);
  CloseHandle(claim);
  return check(result);
#else
  return check(avio_open(io, utf8.c_str(), AVIO_FLAG_WRITE));
#endif
}

struct Encoder {
  std::filesystem::path path, mkv;
  AVFormatContext* format = nullptr;
  AVCodecContext *video = nullptr, *audio = nullptr;
  AVStream *vs = nullptr, *as = nullptr;
  AVFrame *vf = nullptr, *af = nullptr;
  SwsContext* scale = nullptr;
  AVAudioFifo* fifo = nullptr;
  int64_t video_pts = 0, audio_pts = 0;
  bool done = false;
  Failure failure;
  // Independent mailboxes can submit later-captured audio before earlier video
  // (and vice versa). Studio supplies the source clock epoch. Standalone serial
  // producers without that information retain their first-media origin.
  std::optional<std::chrono::steady_clock::time_point> start;

  int64_t elapsedUs(std::chrono::steady_clock::time_point captured) {
    if (!start) {
      start = captured;
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(captured - *start).count();
  }

  Encoder(const std::filesystem::path& p, int w, int h, int fps,
          std::optional<std::chrono::steady_clock::time_point> origin)
      : path(p), mkv(p), start(origin) {
    auto initialized = initialize(w, h, fps);
    if (!initialized) {
      failure = initialized.failure_record();
      release();
    }
  }

  Result<void> initialize(int w, int h, int fps) {
    if (w < 2 || h < 2 || w > 3840 || h > 3840 || fps < 1 || fps > 120) {
      return failed("Invalid recorder dimensions or frame rate", ErrorCode::invalid_argument);
    }
    const auto& p = path;
    LK_STUDIO_TRY(path8(path));
    mkv.replace_extension(".mkv");
    LK_STUDIO_ASSIGN(mkv_name, path8(mkv));
    std::error_code ec;
    const bool output_exists = std::filesystem::exists(path, ec);
    if (ec) {
      return failed(ec.message(), ErrorCode::processing_failed, ec.value());
    }
    const bool intermediate_exists = std::filesystem::exists(mkv, ec);
    if (ec) {
      return failed(ec.message(), ErrorCode::processing_failed, ec.value());
    }
    if (output_exists || intermediate_exists) {
      return failed("录制文件已存在", ErrorCode::invalid_state);
    }
    if (!p.parent_path().empty()) {
      std::filesystem::create_directories(p.parent_path(), ec);
    }
    if (ec) {
      return failed(ec.message(), ErrorCode::processing_failed, ec.value());
    }
    {
      LK_STUDIO_TRY(
          check(avformat_alloc_output_context2(&format, nullptr, "matroska", mkv_name.c_str())));
      auto* vc = avcodec_find_encoder_by_name("libx264");
      if (!vc)
        vc = avcodec_find_encoder(AV_CODEC_ID_H264);
      auto* ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
      if (!vc || !ac)
        return failed("FFmpeg 缺少 H.264/AAC 编码器");
      video = avcodec_alloc_context3(vc);
      audio = avcodec_alloc_context3(ac);
      if (!video || !audio)
        return failed("Recorder allocation failed", ErrorCode::resource_exhausted);
      video->width = w;
      video->height = h;
      video->pix_fmt = AV_PIX_FMT_YUV420P;
      video->time_base = {1, fps};
      video->framerate = {fps, 1};
      video->bit_rate = 4000000;
      video->gop_size = fps * 2;
      video->max_b_frames = 0;
      video->thread_count = 2;
      if (video->priv_data) {
        av_opt_set(video->priv_data, "preset", "veryfast", 0);
        av_opt_set(video->priv_data, "tune", "zerolatency", 0);
      }
      audio->sample_rate = 48000;
      audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
      audio->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
      audio->time_base = {1, 48000};
      audio->bit_rate = 128000;
      if (format->oformat->flags & AVFMT_GLOBALHEADER) {
        video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
      }
      LK_STUDIO_TRY(check(avcodec_open2(video, vc, nullptr)));
      LK_STUDIO_TRY(check(avcodec_open2(audio, ac, nullptr)));
      vs = avformat_new_stream(format, nullptr);
      as = avformat_new_stream(format, nullptr);
      if (!vs || !as)
        return failed("Recorder allocation failed", ErrorCode::resource_exhausted);
      vs->time_base = video->time_base;
      as->time_base = audio->time_base;
      LK_STUDIO_TRY(check(avcodec_parameters_from_context(vs->codecpar, video)));
      LK_STUDIO_TRY(check(avcodec_parameters_from_context(as->codecpar, audio)));
      LK_STUDIO_TRY(openStage(&format->pb, mkv, mkv_name));
      LK_STUDIO_TRY(check(avformat_write_header(format, nullptr)));
      vf = av_frame_alloc();
      af = av_frame_alloc();
      if (!vf || !af)
        return failed("Recorder allocation failed", ErrorCode::resource_exhausted);
      vf->format = video->pix_fmt;
      vf->width = w;
      vf->height = h;
      LK_STUDIO_TRY(check(av_frame_get_buffer(vf, 32)));
      af->format = audio->sample_fmt;
      af->sample_rate = 48000;
      LK_STUDIO_TRY(check(av_channel_layout_copy(&af->ch_layout, &audio->ch_layout)));
      af->nb_samples = audio->frame_size;
      LK_STUDIO_TRY(check(av_frame_get_buffer(af, 0)));
      fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 4096);
      if (!fifo)
        return failed("Recorder allocation failed", ErrorCode::resource_exhausted);
      scale = sws_getContext(w, h, AV_PIX_FMT_RGBA, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr,
                             nullptr, nullptr);
      if (!scale)
        return failed("录制颜色转换失败");
    }
    return Result<void>::success();
  }

  void release() {
    if (format) {
      if (format->pb)
        avio_closep(&format->pb);
      avformat_free_context(format);
      format = nullptr;
    }
    avcodec_free_context(&video);
    avcodec_free_context(&audio);
    av_frame_free(&vf);
    av_frame_free(&af);
    sws_freeContext(scale);
    scale = nullptr;
    if (fifo)
      av_audio_fifo_free(fifo);
    fifo = nullptr;
  }

  ~Encoder() {
    release();
  }

  Result<void> encode(AVCodecContext* c, AVStream* s, AVFrame* f) {
    LK_STUDIO_TRY(check(avcodec_send_frame(c, f)));
    AVPacket* p = av_packet_alloc();
    if (!p)
      return failed("Recorder allocation failed", ErrorCode::resource_exhausted);

    struct PacketGuard {
      AVPacket*& value;

      ~PacketGuard() {
        av_packet_free(&value);
      }
    } guard{p};

    {
      for (;;) {
        int e = avcodec_receive_packet(c, p);
        if (e == AVERROR(EAGAIN) || e == AVERROR_EOF)
          break;
        LK_STUDIO_TRY(check(e));
        av_packet_rescale_ts(p, c->time_base, s->time_base);
        p->stream_index = s->index;
        LK_STUDIO_TRY(check(av_interleaved_write_frame(format, p)));
        av_packet_unref(p);
      }
    }
    return Result<void>::success();
  }

  Result<void> writeVideo(const StudioImage& i, std::chrono::steady_clock::time_point captured) {
    if (i.width != video->width || i.height != video->height ||
        i.rgba.size() < size_t(i.width) * i.height * 4)
      return failed("Invalid recorder video frame", ErrorCode::invalid_argument);
    LK_STUDIO_TRY(check(av_frame_make_writable(vf)));
    const uint8_t* data = i.rgba.data();
    int stride = i.width * 4;
    if (sws_scale(scale, &data, &stride, 0, i.height, vf->data, vf->linesize) != vf->height)
      return failed("Recording color conversion failed");
    auto elapsed = elapsedUs(captured);
    int64_t tick = av_rescale_q(elapsed, {1, 1000000}, video->time_base);
    vf->pts = std::max(video_pts, tick);
    video_pts = vf->pts + 1;
    return encode(video, vs, vf);
  }

  Result<void> writeAudio(livekit::compat::span<const float> samples,
                          std::chrono::steady_clock::time_point captured) {
    if (samples.size() % 2 || samples.size() > 96000)
      return failed("Invalid recorder audio frame", ErrorCode::invalid_argument);
    auto elapsed = elapsedUs(captured);
    int64_t target = elapsed * 48000 / 1000000;
    int64_t queued = audio_pts + av_audio_fifo_size(fifo);
    size_t count = samples.size() / 2;
    if (target + int64_t(count) <= queued)
      return Result<void>::success();
    if (target < queued) {
      auto skip = size_t(queued - target);
      samples = samples.subspan(skip * 2);
      count -= skip;
      target = queued;
    }
    if (target - queued > 240000)
      return failed("Recording audio clock jumped by over five seconds; MKV retained");
    // Align PCM to the same captured timestamp as video, including exact silence
    // gaps. Encoder scheduling latency must never shift the audio timeline.
    if (target > queued) {
      std::vector<float> silence(4800);
      void* planes[]{silence.data(), silence.data()};
      while (target > queued) {
        int n = int(std::min<int64_t>(target - queued, 4800));
        if (av_audio_fifo_write(fifo, planes, n) != n)
          return failed("Recording audio FIFO write failed");
        LK_STUDIO_TRY(drain(false));
        queued += n;
      }
    }
    std::vector<float> l(count), r(count);
    for (size_t i = 0; i < count; ++i) {
      l[i] = samples[i * 2];
      r[i] = samples[i * 2 + 1];
    }
    void* planes[]{l.data(), r.data()};
    if (av_audio_fifo_write(fifo, planes, int(count)) != int(count))
      return failed("Recording audio FIFO write failed");
    return drain(false);
  }

  Result<void> drain(bool final) {
    while (av_audio_fifo_size(fifo) >= af->nb_samples || (final && av_audio_fifo_size(fifo) > 0)) {
      LK_STUDIO_TRY(check(av_frame_make_writable(af)));
      int n = std::min(av_audio_fifo_size(fifo), af->nb_samples);
      for (int c = 0; c < 2; ++c)
        memset(af->data[c], 0, size_t(af->nb_samples) * sizeof(float));
      if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(af->data), n) != n)
        return failed("Recording audio FIFO read failed");
      af->pts = audio_pts;
      audio_pts += af->nb_samples;
      LK_STUDIO_TRY(encode(audio, as, af));
    }
    return Result<void>::success();
  }

  Result<void> finish() {
    if (done)
      return Result<void>::success();
    LK_STUDIO_TRY(drain(true));
    LK_STUDIO_TRY(encode(video, vs, nullptr));
    LK_STUDIO_TRY(encode(audio, as, nullptr));
    LK_STUDIO_TRY(check(av_write_trailer(format)));
    LK_STUDIO_TRY(check(avio_closep(&format->pb)));
    done = true;
    AVFormatContext *in = nullptr, *out = nullptr;
    AVPacket* p = nullptr;
    auto temp = path;
    temp += ".partial";
    LK_STUDIO_ASSIGN(mkv_name, path8(mkv));
    LK_STUDIO_ASSIGN(temp_name, path8(temp));

    struct RemuxGuard {
      AVFormatContext*& in;
      AVFormatContext*& out;
      AVPacket*& packet;

      ~RemuxGuard() {
        av_packet_free(&packet);
        avformat_close_input(&in);
        if (out) {
          if (out->pb) {
            avio_closep(&out->pb);
          }
          avformat_free_context(out);
        }
      }
    } guard{in, out, p};

    {
      LK_STUDIO_TRY(check(avformat_open_input(&in, mkv_name.c_str(), nullptr, nullptr)));
      LK_STUDIO_TRY(check(avformat_find_stream_info(in, nullptr)));
      LK_STUDIO_TRY(check(avformat_alloc_output_context2(&out, nullptr, "mp4", temp_name.c_str())));
      for (unsigned i = 0; i < in->nb_streams; ++i) {
        auto* s = avformat_new_stream(out, nullptr);
        if (!s)
          return failed("Recorder allocation failed", ErrorCode::resource_exhausted);
        LK_STUDIO_TRY(check(avcodec_parameters_copy(s->codecpar, in->streams[i]->codecpar)));
        s->codecpar->codec_tag = 0;
        s->time_base = in->streams[i]->time_base;
      }
      LK_STUDIO_TRY(openStage(&out->pb, temp, temp_name));
      LK_STUDIO_TRY(check(avformat_write_header(out, nullptr)));
      p = av_packet_alloc();
      if (!p)
        return failed("Recorder allocation failed", ErrorCode::resource_exhausted);
      int e;
      while ((e = av_read_frame(in, p)) >= 0) {
        auto index = p->stream_index;
        av_packet_rescale_ts(p, in->streams[index]->time_base, out->streams[index]->time_base);
        p->pos = -1;
        LK_STUDIO_TRY(check(av_interleaved_write_frame(out, p)));
        av_packet_unref(p);
      }
      if (e != AVERROR_EOF)
        LK_STUDIO_TRY(check(e));
      LK_STUDIO_TRY(check(av_write_trailer(out)));
      LK_STUDIO_TRY(check(avio_closep(&out->pb)));
      av_packet_free(&p);
      avformat_close_input(&in);
      avformat_free_context(out);
      out = nullptr;
#ifdef _WIN32
      // A different session may have published this name since initialize().
      if (!MoveFileW(temp.c_str(), path.c_str())) {
        const auto code = GetLastError();
        return failed("Cannot publish recording without replacing the destination",
                      ErrorCode::processing_failed, code);
      }
#else
      std::error_code ec;
      std::filesystem::rename(temp, path, ec);
      if (ec) {
        return failed(ec.message(), ErrorCode::processing_failed, ec.value());
      }
#endif
    }
    return Result<void>::success();
  }
};

struct Recorder::Impl {
  Encoder encoder;
  bool closing = false;
  Failure error;

  Impl(const std::filesystem::path& path, int width, int height, int fps,
       std::optional<std::chrono::steady_clock::time_point> origin)
      : encoder(path, width, height, fps, origin), error(encoder.failure) {}

  ~Impl() {
    (void)finish();
  }

  template <class Write>
  Result<void> write_here(Write&& write) {
    if (error)
      return failed(error);
    if (closing)
      return failed("Recorder is closed");
    auto result = write();
    if (!result)
      error = result.failure_record();
    return result;
  }

  Result<void> finish() {
    if (!closing && !error) {
      auto result = encoder.finish();
      if (!result) {
        error = result.failure_record();
      }
    }
    closing = true;
    return error ? Result<void>::failure(error) : Result<void>::success();
  }
};

Recorder::Recorder(const std::filesystem::path& p, int w, int h, int fps,
                   std::optional<std::chrono::steady_clock::time_point> origin)
    : impl_(std::make_unique<Impl>(p, w, h, fps, origin)) {}

Recorder::~Recorder() = default;

void Recorder::setMediaOrigin(std::chrono::steady_clock::time_point origin) {
  impl_->encoder.start = origin;
}

Result<void> Recorder::status() const {
  return impl_->error ? Result<void>::failure(impl_->error) : Result<void>::success();
}

Result<void> Recorder::video(const StudioImage& i, std::chrono::steady_clock::time_point captured) {
  return impl_->write_here([&] { return impl_->encoder.writeVideo(i, captured); });
}

Result<void> Recorder::audio(livekit::compat::span<const float> s,
                             std::chrono::steady_clock::time_point captured) {
  return impl_->write_here([&] { return impl_->encoder.writeAudio(s, captured); });
}

Result<void> Recorder::finish() {
  return impl_->finish();
}
}  // namespace livekit::sdk::studio_detail
