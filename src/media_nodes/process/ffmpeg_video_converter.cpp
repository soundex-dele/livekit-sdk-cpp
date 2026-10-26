#include "backends/ffmpeg/ffmpeg_playback.hpp"

namespace livekit::sdk::detail {
VideoConverterNode::~VideoConverterNode() { doRelease(); }
bool VideoConverterNode::doInit() {
  subscribe_media<DecodedVideo>([this](const DecodedVideo& event) {
    if (input_ || finished_) return false;
    input_ = event; return true;
  });
  return true;
}
void VideoConverterNode::doRelease() { input_.reset(); pending_.reset(); sws_freeContext(scaler_); scaler_ = nullptr; }
void VideoConverterNode::flush(int64_t position) { doRelease(); discard_before_ = position; }
std::optional<media::VideoFrame> VideoConverterNode::convert(const DecodedFrame& input) {
  if (input.timestamp_us < discard_before_) return {};
  const auto* frame = input.frame.get();
  if (!frame) throw PlayerError("playback/video.rgba: missing decoded frame");
  const int width = frame->width, height = frame->height;
  if (width <= 0 || height <= 0 || static_cast<int64_t>(width) * height > 33554432)
    throw PlayerError("playback/video.rgba: unsupported dimensions (limit 32M pixels)");
  scaler_ = sws_getCachedContext(scaler_, width, height, static_cast<AVPixelFormat>(frame->format),
      width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!scaler_) throw PlayerError("playback/video.rgba: cannot create converter");
  const int colorspace = frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 :
      frame->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_DEFAULT;
  ffmpeg_check(sws_setColorspaceDetails(scaler_, sws_getCoefficients(colorspace),
      frame->color_range == AVCOL_RANGE_JPEG, sws_getCoefficients(colorspace), 1, 0, 1 << 16, 1 << 16), "video.rgba/colorspace");
  auto data = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * height * 4);
  uint8_t* planes[4] = {data->data()}; int strides[4] = {width * 4};
  ffmpeg_check(sws_scale(scaler_, frame->data, frame->linesize, 0, height, planes, strides), "video.rgba/convert");
  media::VideoFrame result;
  result.width = width; result.height = height; result.stride = width * 4;
  result.timestamp_us = input.timestamp_us; result.duration_us = input.duration_us;
  result.rgba = std::move(data);
  return result;
}
void VideoConverterNode::on_reset(int64_t position) { flush(position); }
bool VideoConverterNode::pump() {
  if (finished_) return false;
  if (pending_) {
    if (!publish_media(*pending_)) return false;
    finished_ = pending_->eos; pending_.reset(); return true;
  }
  if (!input_) return false;
  RGBA8 event; event.eos = input_->eos;
  if (event.eos) pending_ = event;
  else if (auto frame = convert(input_->frame)) { event.frame = std::move(*frame); pending_ = std::move(event); }
  input_.reset(); return true;
}
}
