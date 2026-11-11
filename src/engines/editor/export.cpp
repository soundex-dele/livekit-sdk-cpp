#include "editor_internal.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#include <algorithm>

namespace livekit::sdk::editor_detail {
namespace {
class Writer {
 public:
  ~Writer() {
    av_frame_free(&video_frame);
    av_frame_free(&audio_frame);
    av_packet_free(&packet);
    avcodec_free_context(&video);
    avcodec_free_context(&audio);
    sws_freeContext(scaler);
    if (format) {
      if (format->pb) {
        avio_closep(&format->pb);
      }
      avformat_free_context(format);
    }
  }

  Result<void> checked(int rc, const std::string& operation) {
    if (rc >= 0) {
      return Result<void>::success();
    }
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(rc, text, sizeof(text));
    return Result<void>::failure(failure("export/" + operation + ": " + text));
  }

  Result<void> open(const EditorProject& p, const std::filesystem::path& path) {
    const AVCodec* vc = avcodec_find_encoder_by_name("libx264");
    if (!vc) {
      vc = avcodec_find_encoder_by_name("libopenh264");
    }
    const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!vc || !ac) {
      return Result<void>::failure(
          failure("export: software H.264 / AAC encoder unavailable", ErrorCode::unsupported));
    }
    auto result =
        checked(avformat_alloc_output_context2(&format, nullptr, "mp4", path.u8string().c_str()),
                "create MP4");
    if (!result) {
      return result;
    }
    video = avcodec_alloc_context3(vc);
    audio = avcodec_alloc_context3(ac);
    if (!format || !video || !audio) {
      return Result<void>::failure(failure("export: allocation failed"));
    }
    video->width = p.width;
    video->height = p.height;
    video->time_base = {p.rate.denominator, p.rate.numerator};
    video->framerate = {p.rate.numerator, p.rate.denominator};
    video->pix_fmt = AV_PIX_FMT_YUV420P;
    video->gop_size = 60;
    video->thread_count = 2;
    video->bit_rate = 8000000;
    video->color_range = AVCOL_RANGE_MPEG;
    video->colorspace = AVCOL_SPC_BT709;
    video->color_primaries = AVCOL_PRI_BT709;
    video->color_trc = AVCOL_TRC_BT709;
    audio->time_base = {1, 48000};
    audio->sample_rate = 48000;
    audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
    audio->bit_rate = 192000;
    av_channel_layout_default(&audio->ch_layout, 2);
    if (format->oformat->flags & AVFMT_GLOBALHEADER) {
      video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
      audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    AVDictionary* options = nullptr;
    if (std::string(vc->name) == "libx264") {
      av_dict_set(&options, "preset", "veryfast", 0);
      av_dict_set(&options, "crf", "18", 0);
    }
    result = checked(avcodec_open2(video, vc, &options), "open H.264");
    av_dict_free(&options);
    if (!result) {
      return result;
    }
    result = checked(avcodec_open2(audio, ac, nullptr), "open AAC");
    if (!result) {
      return result;
    }
    video_stream = avformat_new_stream(format, nullptr);
    audio_stream = avformat_new_stream(format, nullptr);
    if (!video_stream || !audio_stream) {
      return Result<void>::failure(failure("export: stream allocation failed"));
    }
    video_stream->time_base = video->time_base;
    audio_stream->time_base = audio->time_base;
    result =
        checked(avcodec_parameters_from_context(video_stream->codecpar, video), "video parameters");
    if (!result) {
      return result;
    }
    result =
        checked(avcodec_parameters_from_context(audio_stream->codecpar, audio), "audio parameters");
    if (!result) {
      return result;
    }
    result =
        checked(avio_open(&format->pb, path.u8string().c_str(), AVIO_FLAG_WRITE), "open output");
    if (!result) {
      return result;
    }
    av_dict_set(&options, "movflags", "+faststart", 0);
    result = checked(avformat_write_header(format, &options), "write header");
    av_dict_free(&options);
    if (!result) {
      return result;
    }
    video_frame = av_frame_alloc();
    audio_frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!video_frame || !audio_frame || !packet) {
      return Result<void>::failure(failure("export: frame allocation failed"));
    }
    video_frame->format = video->pix_fmt;
    video_frame->width = p.width;
    video_frame->height = p.height;
    result = checked(av_frame_get_buffer(video_frame, 32), "video buffer");
    if (!result) {
      return result;
    }
    scaler = sws_getContext(p.width, p.height, AV_PIX_FMT_RGBA, p.width, p.height, video->pix_fmt,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scaler) {
      return Result<void>::failure(failure("export: scaler failed"));
    }
    sws_setColorspaceDetails(scaler, sws_getCoefficients(SWS_CS_ITU709), 1,
                             sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);
    return Result<void>::success();
  }

  Result<void> encode(AVCodecContext* codec, AVStream* stream, AVFrame* frame) {
    auto result = checked(avcodec_send_frame(codec, frame), "send frame");
    if (!result) {
      return result;
    }
    for (;;) {
      int rc = avcodec_receive_packet(codec, packet);
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
        return Result<void>::success();
      }
      result = checked(rc, "receive packet");
      if (!result) {
        return result;
      }
      av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
      packet->stream_index = stream->index;
      result = checked(av_interleaved_write_frame(format, packet), "mux packet");
      av_packet_unref(packet);
      if (!result) {
        return result;
      }
    }
  }

  Result<void> audio_samples(bool flush) {
    const int frame_size = audio->frame_size;
    while (pending.size() / 2 >= size_t(frame_size) || (flush && !pending.empty())) {
      const int count = std::min(frame_size, int(pending.size() / 2));
      av_frame_unref(audio_frame);
      audio_frame->format = audio->sample_fmt;
      audio_frame->sample_rate = audio->sample_rate;
      audio_frame->nb_samples = count;
      av_channel_layout_copy(&audio_frame->ch_layout, &audio->ch_layout);
      auto result = checked(av_frame_get_buffer(audio_frame, 0), "audio buffer");
      if (!result) {
        return result;
      }
      for (int ch = 0; ch < 2; ++ch) {
        auto* plane = reinterpret_cast<float*>(audio_frame->data[ch]);
        for (int i = 0; i < count; ++i) {
          plane[i] = pending[size_t(i) * 2 + ch];
        }
      }
      audio_frame->pts = audio_position;
      audio_position += count;
      result = encode(audio, audio_stream, audio_frame);
      if (!result) {
        return result;
      }
      pending.erase(pending.begin(), pending.begin() + count * 2);
    }
    return Result<void>::success();
  }

  Result<void> write(const EditorFrame& frame) {
    auto result = checked(av_frame_make_writable(video_frame), "writable frame");
    if (!result) {
      return result;
    }
    const uint8_t* data[]{frame.image.rgba.data()};
    int strides[]{frame.image.width * 4};
    result = checked(sws_scale(scaler, data, strides, 0, frame.image.height, video_frame->data,
                               video_frame->linesize),
                     "convert video");
    if (!result) {
      return result;
    }
    video_frame->pts = frame.frame;
    result = encode(video, video_stream, video_frame);
    if (!result) {
      return result;
    }
    pending.insert(pending.end(), frame.audio.begin(), frame.audio.end());
    return audio_samples(false);
  }

  Result<void> finish() {
    auto result = audio_samples(true);
    if (!result) {
      return result;
    }
    result = encode(video, video_stream, nullptr);
    if (!result) {
      return result;
    }
    result = encode(audio, audio_stream, nullptr);
    if (!result) {
      return result;
    }
    result = checked(av_write_trailer(format), "write trailer");
    if (!result) {
      return result;
    }
    return checked(avio_closep(&format->pb), "close output");
  }

 private:
  AVFormatContext* format = nullptr;
  AVCodecContext* video = nullptr;
  AVCodecContext* audio = nullptr;
  AVStream* video_stream = nullptr;
  AVStream* audio_stream = nullptr;
  AVFrame* video_frame = nullptr;
  AVFrame* audio_frame = nullptr;
  AVPacket* packet = nullptr;
  SwsContext* scaler = nullptr;
  std::vector<float> pending;
  int64_t audio_position = 0;
};
}  // namespace

Result<void> export_mp4(const EditorProject& p, const std::filesystem::path& path,
                        std::atomic<bool>& cancel,
                        const std::function<void(EditorExportProgress)>& progress) {
  auto valid = editor_validate(p);
  if (!valid) {
    return valid;
  }
  const auto total = editor_duration(p);
  if (total <= 0 || path.empty()) {
    return Result<void>::failure(failure("export: empty timeline/output"));
  }
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  if (ec || !std::filesystem::is_directory(absolute.parent_path(), ec)) {
    return Result<void>::failure(failure("export: output directory does not exist"));
  }
  for (const auto& asset : p.assets) {
    if (std::filesystem::equivalent(std::filesystem::u8path(asset.path), absolute, ec)) {
      return Result<void>::failure(failure("export: output would overwrite a source asset"));
    }
    ec.clear();
  }
  auto temp = absolute;
  temp += "." + std::to_string(next_failure_id()) + ".tmp.mp4";

  struct Cleanup {
    std::filesystem::path path;

    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  } cleanup{temp};

  Writer writer;
  auto result = writer.open(p, temp);
  if (!result) {
    return result;
  }
  RenderGraph graph(p, cancel);
  for (int64_t frame = 0; frame < total; ++frame) {
    if (cancel) {
      return Result<void>::failure(failure("export: cancelled", ErrorCode::cancelled));
    }
    auto rendered = graph.render(frame, 0);
    if (!rendered) {
      return Result<void>::failure(rendered.failure_record());
    }
    result = writer.write(**rendered);
    if (!result) {
      return result;
    }
    if (progress &&
        (frame == 0 || frame + 1 == total || frame % std::max<int64_t>(1, total / 1000) == 0)) {
      progress({frame + 1, total});
    }
  }
  result = writer.finish();
  if (!result) {
    return result;
  }
  if (cancel) {
    return Result<void>::failure(failure("export: cancelled", ErrorCode::cancelled));
  }
  return atomic_replace(temp, absolute);
}
}  // namespace livekit::sdk::editor_detail
