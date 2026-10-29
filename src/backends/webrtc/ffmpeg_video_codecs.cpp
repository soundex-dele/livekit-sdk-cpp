#include "backends/webrtc/ffmpeg_video_codecs.hpp"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/video_codec.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace livekit::sdk::detail {
namespace {
using namespace webrtc;

AVCodecID codec_id(const SdpVideoFormat& f) {
  auto name = f.name;
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (name == "H264")
    return AV_CODEC_ID_H264;
  if (name == "H265")
    return AV_CODEC_ID_HEVC;
  return AV_CODEC_ID_NONE;
}

const AVCodec* encoder_for(AVCodecID id) {
  return avcodec_find_encoder_by_name(id == AV_CODEC_ID_H264 ? "libx264" : "libx265");
}

std::vector<SdpVideoFormat> formats(bool encode) {
  std::vector<SdpVideoFormat> result;
  if (encode ? encoder_for(AV_CODEC_ID_H264) != nullptr
             : avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr)
    result.emplace_back("H264", SdpVideoFormat::Parameters{{"profile-level-id", "42e01f"},
                                                           {"packetization-mode", "1"},
                                                           {"level-asymmetry-allowed", "1"}});
  if (encode ? encoder_for(AV_CODEC_ID_HEVC) != nullptr
             : avcodec_find_decoder(AV_CODEC_ID_HEVC) != nullptr)
    result.emplace_back(
        "H265",
        SdpVideoFormat::Parameters{
            {"profile-id", "1"}, {"tier-flag", "0"}, {"level-id", "93"}, {"tx-mode", "SRST"}});
  return result;
}

bool supported(const SdpVideoFormat& f, bool encode) {
  return f.IsCodecInList(formats(encode));
}

void copy_plane(uint8_t* to, int to_stride, const uint8_t* from, int from_stride, int width,
                int height) {
  for (int y = 0; y < height; ++y)
    std::memcpy(to + y * to_stride, from + y * from_stride, width);
}

class Encoder final : public VideoEncoder {
 public:
  explicit Encoder(AVCodecID id) : id_(id) {}

  ~Encoder() override {
    Release();
  }

  int InitEncode(const VideoCodec* c, const Settings&) override {
    Release();
    if (!c || c->width < 2 || c->height < 2 || c->width > 1280 || c->height > 720 ||
        c->maxFramerate > 30 || c->width % 2 || c->height % 2 || c->numberOfSimulcastStreams > 1 ||
        (c->GetScalabilityMode() && *c->GetScalabilityMode() != ScalabilityMode::kL1T1))
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    width_ = c->width;
    height_ = c->height;
    fps_ = std::clamp<int>(c->maxFramerate, 1, 30);
    bitrate_ = std::max<uint32_t>(c->startBitrate * 1000, 10000);
    paused_ = false;
    return open() ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_ERROR;
  }

  int32_t RegisterEncodeCompleteCallback(EncodedImageCallback* cb) override {
    callback_ = cb;
    return 0;
  }

  int32_t Release() override {
    avcodec_free_context(&context_);
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    return 0;
  }

  void SetRates(const RateControlParameters& rates) override {
    const auto next = rates.bitrate.get_sum_bps();
    paused_ = next == 0;
    if (paused_)
      return;
    const auto fps =
        rates.framerate_fps > 0 ? std::clamp<int>(int(rates.framerate_fps + .5), 1, 30) : fps_;
    // x265 does not apply AVCodecContext bit_rate changes dynamically. Reopen
    // on material changes, producing new parameter sets and an IDR. Minor
    // changes are accumulated relative to the rate used for the open context.
    if (next < bitrate_ * .8 || next > bitrate_ * 1.2 || fps != fps_) {
      bitrate_ = next;
      fps_ = fps;
      reconfigure_ = true;
    }
  }

  int32_t Encode(const VideoFrame& input, const std::vector<VideoFrameType>* types) override {
    if (!context_ || !callback_)
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    if (paused_) {
      callback_->OnDroppedFrame(EncodedImageCallback::DropReason::kDroppedByEncoder);
      return 0;
    }
    if (reconfigure_) {
      Release();
      if (!open())
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    const auto pixels = input.video_frame_buffer()->ToI420();
    if (!pixels || pixels->width() != width_ || pixels->height() != height_)
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    if (av_frame_make_writable(frame_) < 0)
      return WEBRTC_VIDEO_CODEC_ERROR;
    copy_plane(frame_->data[0], frame_->linesize[0], pixels->DataY(), pixels->StrideY(), width_,
               height_);
    copy_plane(frame_->data[1], frame_->linesize[1], pixels->DataU(), pixels->StrideU(), width_ / 2,
               height_ / 2);
    copy_plane(frame_->data[2], frame_->linesize[2], pixels->DataV(), pixels->StrideV(), width_ / 2,
               height_ / 2);
    // Keep codec PTS monotonic across RTP's 32-bit wrap. RTP is attached to the
    // immediate output; these encoders use zerolatency and never reorder frames.
    frame_->pts = frame_index_++;
    const bool key =
        force_key_ || (types && std::find(types->begin(), types->end(),
                                          VideoFrameType::kVideoFrameKey) != types->end());
    frame_->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (avcodec_send_frame(context_, frame_) < 0)
      return WEBRTC_VIDEO_CODEC_ERROR;
    int rc = avcodec_receive_packet(context_, packet_);
    if (rc < 0)
      return WEBRTC_VIDEO_CODEC_ERROR;  // no delayed output allowed
    EncodedImage image;
    image.SetEncodedData(EncodedImageBuffer::Create(packet_->data, packet_->size));
    image.SetRtpTimestamp(input.rtp_timestamp());
    image._encodedWidth = width_;
    image._encodedHeight = height_;
    image.capture_time_ms_ = input.timestamp_us() / 1000;
    image._frameType = packet_->flags & AV_PKT_FLAG_KEY ? VideoFrameType::kVideoFrameKey
                                                        : VideoFrameType::kVideoFrameDelta;
    CodecSpecificInfo info;
    info.codecType = id_ == AV_CODEC_ID_H264 ? kVideoCodecH264 : kVideoCodecH265;
    if (id_ == AV_CODEC_ID_H264) {
      info.codecSpecific.H264.packetization_mode = H264PacketizationMode::NonInterleaved;
      info.codecSpecific.H264.temporal_idx = 0;
      info.codecSpecific.H264.base_layer_sync = false;
      info.codecSpecific.H264.idr_frame = image._frameType == VideoFrameType::kVideoFrameKey;
    }
    auto result = callback_->OnEncodedImage(image, &info);
    av_packet_unref(packet_);
    force_key_ = false;
    return result.error == EncodedImageCallback::Result::OK ? 0 : WEBRTC_VIDEO_CODEC_ERROR;
  }

  EncoderInfo GetEncoderInfo() const override {
    EncoderInfo i;
    i.implementation_name = id_ == AV_CODEC_ID_H264 ? "FFmpeg/libx264" : "FFmpeg/libx265";
    i.is_hardware_accelerated = false;
    i.supports_native_handle = false;
    i.has_trusted_rate_controller = false;
    return i;
  }

 private:
  bool open() {
    const auto* codec = encoder_for(id_);
    if (!codec)
      return false;
    context_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!context_ || !frame_ || !packet_) {
      Release();
      return false;
    }
    context_->width = width_;
    context_->height = height_;
    context_->pix_fmt = AV_PIX_FMT_YUV420P;
    context_->time_base = AVRational{1, fps_};
    context_->framerate = AVRational{fps_, 1};
    context_->bit_rate = bitrate_;
    context_->rc_max_rate = bitrate_;
    context_->rc_buffer_size = int(bitrate_);
    context_->gop_size = fps_ * 2;
    context_->max_b_frames = 0;
    context_->thread_count = 1;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", "ultrafast", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
    av_dict_set(&opts, "forced-idr", "1", 0);
    if (id_ == AV_CODEC_ID_H264) {
      // These names also exist as numeric AVCodecContext options. Address
      // libx264's private string options directly to avoid the name collision.
      if (av_opt_set(context_->priv_data, "profile", "baseline", 0) < 0 ||
          av_opt_set(context_->priv_data, "level", "3.1", 0) < 0) {
        av_dict_free(&opts);
        Release();
        return false;
      }
      av_dict_set(&opts, "x264-params", "repeat-headers=1:annexb=1:scenecut=0", 0);
    } else {
      av_dict_set(&opts, "x265-params",
                  "repeat-headers=1:annexb=1:open-gop=0:bframes=0:rc-lookahead=0:frame-threads=1:"
                  "pools=none:log-level=error",
                  0);
    }
    const int rc = avcodec_open2(context_, codec, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
      Release();
      return false;
    }
    frame_->format = context_->pix_fmt;
    frame_->width = width_;
    frame_->height = height_;
    if (av_frame_get_buffer(frame_, 32) < 0) {
      Release();
      return false;
    }
    force_key_ = true;
    reconfigure_ = false;
    frame_index_ = 0;
    return true;
  }

  AVCodecID id_;
  AVCodecContext* context_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  EncodedImageCallback* callback_ = nullptr;
  int width_ = 0, height_ = 0, fps_ = 30;
  uint32_t bitrate_ = 1000000;
  int64_t frame_index_ = 0;
  bool paused_ = false, force_key_ = true, reconfigure_ = false;
};

class Decoder final : public VideoDecoder {
 public:
  explicit Decoder(AVCodecID id) : id_(id) {}

  ~Decoder() override {
    Release();
  }

  bool Configure(const Settings& settings) override {
    Release();
    const auto* codec = avcodec_find_decoder(id_);
    if (!codec)
      return false;
    context_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!context_ || !frame_ || !packet_) {
      Release();
      return false;
    }
    context_->thread_count = 1;
    context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context_->max_pixels = 1920 * 1080;
    if (avcodec_open2(context_, codec, nullptr) < 0) {
      Release();
      return false;
    }
    return true;
  }

  int32_t RegisterDecodeCompleteCallback(DecodedImageCallback* cb) override {
    callback_ = cb;
    return 0;
  }

  int32_t Release() override {
    avcodec_free_context(&context_);
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    return 0;
  }

  int32_t Decode(const EncodedImage& image, int64_t) override {
    if (!context_ || !callback_)
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    if (!image.size() || image.size() > 8 * 1024 * 1024)
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    // av_new_packet provides FFmpeg's required zero padding; network buffers do not.
    if (av_new_packet(packet_, int(image.size())) < 0)
      return WEBRTC_VIDEO_CODEC_MEMORY;
    std::memcpy(packet_->data, image.data(), image.size());
    packet_->pts = image.RtpTimestamp();
    const int rc = avcodec_send_packet(context_, packet_);
    av_packet_unref(packet_);
    if (rc < 0)
      return WEBRTC_VIDEO_CODEC_ERROR;
    for (;;) {
      const int got = avcodec_receive_frame(context_, frame_);
      if (got == AVERROR(EAGAIN) || got == AVERROR_EOF)
        break;
      if (got < 0)
        return WEBRTC_VIDEO_CODEC_ERROR;
      if ((frame_->format != AV_PIX_FMT_YUV420P && frame_->format != AV_PIX_FMT_YUVJ420P) ||
          frame_->width <= 0 || frame_->height <= 0 || frame_->width > 1920 ||
          frame_->height > 1080) {
        av_frame_unref(frame_);
        return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
      }
      auto buffer = I420Buffer::Copy(frame_->width, frame_->height, frame_->data[0],
                                     frame_->linesize[0], frame_->data[1], frame_->linesize[1],
                                     frame_->data[2], frame_->linesize[2]);
      auto decoded = VideoFrame::Builder()
                         .set_video_frame_buffer(buffer)
                         .set_rtp_timestamp(static_cast<uint32_t>(frame_->pts))
                         .set_timestamp_us(0)
                         .build();
      callback_->Decoded(decoded);
      av_frame_unref(frame_);
    }
    return 0;
  }

  DecoderInfo GetDecoderInfo() const override {
    return {"FFmpeg/software", false};
  }

 private:
  AVCodecID id_;
  AVCodecContext* context_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  DecodedImageCallback* callback_ = nullptr;
};

template <class Factory>
std::vector<SdpVideoFormat> merged(const Factory& f, bool encode) {
  auto result = f.GetSupportedFormats();
  for (const auto& extra : formats(encode))
    if (!extra.IsCodecInList(result))
      result.push_back(extra);
  return result;
}

class Encoders final : public VideoEncoderFactory {
 public:
  explicit Encoders(std::shared_ptr<VideoEncoderFactory> fallback)
      : fallback_(std::move(fallback)) {}

  std::vector<SdpVideoFormat> GetSupportedFormats() const override {
    return merged(*fallback_, true);
  }

  CodecSupport QueryCodecSupport(const SdpVideoFormat& f,
                                 std::optional<std::string> mode) const override {
    if (supported(f, true))
      return {!mode || *mode == "L1T1", false};
    return fallback_->QueryCodecSupport(f, mode);
  }

  std::unique_ptr<VideoEncoder> Create(const Environment& env, const SdpVideoFormat& f) override {
    if (supported(f, true))
      return std::make_unique<Encoder>(codec_id(f));
    return fallback_->Create(env, f);
  }

 private:
  std::shared_ptr<VideoEncoderFactory> fallback_;
};

class Decoders final : public VideoDecoderFactory {
 public:
  explicit Decoders(std::shared_ptr<VideoDecoderFactory> fallback)
      : fallback_(std::move(fallback)) {}

  std::vector<SdpVideoFormat> GetSupportedFormats() const override {
    return merged(*fallback_, false);
  }

  CodecSupport QueryCodecSupport(const SdpVideoFormat& f, bool reference_scaling) const override {
    if (supported(f, false))
      return {!reference_scaling, false};
    return fallback_->QueryCodecSupport(f, reference_scaling);
  }

  std::unique_ptr<VideoDecoder> Create(const Environment& env, const SdpVideoFormat& f) override {
    if (supported(f, false))
      return std::make_unique<Decoder>(codec_id(f));
    return fallback_->Create(env, f);
  }

 private:
  std::shared_ptr<VideoDecoderFactory> fallback_;
};
}  // namespace

std::shared_ptr<webrtc::VideoEncoderFactory> with_ffmpeg_encoders(
    std::shared_ptr<webrtc::VideoEncoderFactory> fallback) {
  return std::make_shared<Encoders>(std::move(fallback));
}

std::shared_ptr<webrtc::VideoDecoderFactory> with_ffmpeg_decoders(
    std::shared_ptr<webrtc::VideoDecoderFactory> fallback) {
  return std::make_shared<Decoders>(std::move(fallback));
}
}  // namespace livekit::sdk::detail
