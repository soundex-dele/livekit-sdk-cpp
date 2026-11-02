#include "backends/ffmpeg/ffmpeg_playback.hpp"

namespace livekit::sdk::detail {
VideoConverterNode::~VideoConverterNode() {
  doRelease();
}

bool VideoConverterNode::doInit() {
  auto subscribed =
      subscribe_media<DecodedVideo>([this](const DecodedVideo& event) -> Result<bool> {
        if (input_ || finished_) {
          return Result<bool>::success(false);
        }
        input_ = event;
        return Result<bool>::success(true);
      });
  if (!subscribed) {
    reportError(graph_error(subscribed.failure_record()));
    return false;
  }
  return true;
}

void VideoConverterNode::doRelease() {
  input_.reset();
  pending_.reset();
  sws_freeContext(scaler_);
  scaler_ = nullptr;
}

void VideoConverterNode::flush(int64_t position) {
  doRelease();
  discard_before_ = position;
}

Result<std::optional<media::VideoFrame>> VideoConverterNode::convert(const DecodedFrame& input) {
  if (input.timestamp_us < discard_before_) {
    return Result<std::optional<media::VideoFrame>>::success({});
  }
  const auto* frame = input.frame.get();
  if (!frame) {
    return Result<std::optional<media::VideoFrame>>::failure(
        playback_failure("playback/video.rgba: missing decoded frame"));
  }
  const int width = frame->width, height = frame->height;
  if (width <= 0 || height <= 0 || static_cast<int64_t>(width) * height > 33554432) {
    return Result<std::optional<media::VideoFrame>>::failure(
        playback_failure("playback/video.rgba: unsupported dimensions (limit 32M pixels)"));
  }
  scaler_ =
      sws_getCachedContext(scaler_, width, height, static_cast<AVPixelFormat>(frame->format), width,
                           height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!scaler_) {
    return Result<std::optional<media::VideoFrame>>::failure(
        playback_failure("playback/video.rgba: cannot create converter"));
  }
  const int colorspace = frame->colorspace == AVCOL_SPC_BT709        ? SWS_CS_ITU709
                         : frame->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020
                                                                     : SWS_CS_DEFAULT;
  {
    auto checked = ffmpeg_check(
        sws_setColorspaceDetails(scaler_, sws_getCoefficients(colorspace),
                                 frame->color_range == AVCOL_RANGE_JPEG,
                                 sws_getCoefficients(colorspace), 1, 0, 1 << 16, 1 << 16),
        "video.rgba/colorspace");
    if (!checked) {
      return Result<std::optional<media::VideoFrame>>::failure(checked.failure_record());
    }
  }
  auto data = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * height * 4);
  uint8_t* planes[4] = {data->data()};
  int strides[4] = {width * 4};
  {
    auto checked =
        ffmpeg_check(sws_scale(scaler_, frame->data, frame->linesize, 0, height, planes, strides),
                     "video.rgba/convert");
    if (!checked) {
      return Result<std::optional<media::VideoFrame>>::failure(checked.failure_record());
    }
  }
  media::VideoFrame result;
  result.width = width;
  result.height = height;
  result.stride = width * 4;
  result.timestamp_us = input.timestamp_us;
  result.duration_us = input.duration_us;
  result.rgba = std::move(data);
  return Result<std::optional<media::VideoFrame>>::success(result);
}

Result<void> VideoConverterNode::on_reset(int64_t position) {
  flush(position);
  return Result<void>::success();
}

Result<bool> VideoConverterNode::pump() {
  if (finished_) {
    return Result<bool>::success(false);
  }
  if (pending_) {
    auto published = publish_media(*pending_);
    if (!published) {
      return Result<bool>::failure(published.failure_record());
    }
    if (!*published) {
      return Result<bool>::success(false);
    }
    finished_ = pending_->eos;
    pending_.reset();
    return Result<bool>::success(true);
  }
  if (!input_) {
    return Result<bool>::success(false);
  }
  RGBA8 event;
  event.eos = input_->eos;
  if (event.eos) {
    pending_ = event;
  } else {
    auto converted = convert(input_->frame);
    if (!converted) {
      return Result<bool>::failure(converted.failure_record());
    }
    if (auto frame = std::move(*converted)) {
      event.frame = std::move(*frame);
      pending_ = std::move(event);
    }
  }
  input_.reset();
  return Result<bool>::success(true);
}
}  // namespace livekit::sdk::detail
