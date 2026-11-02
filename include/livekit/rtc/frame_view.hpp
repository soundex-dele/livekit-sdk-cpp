#pragma once
#include "livekit/compat/span.hpp"
#include "livekit/media_core/frame_view.hpp"
#include "livekit/rtc/media_types.hpp"

namespace livekit::sdk::rtc {
inline media::FrameView frame_view(std::shared_ptr<const VideoFrame> pixels,
                                   media::FrameTiming timing) {
  media::FrameView view;
  view.owner = pixels;
  view.format = media::FrameFormat::i420;
  view.width = pixels->width;
  view.height = pixels->height;
  view.planes = {livekit::compat::as_bytes(livekit::compat::span(pixels->y)),
                 livekit::compat::as_bytes(livekit::compat::span(pixels->u)),
                 livekit::compat::as_bytes(livekit::compat::span(pixels->v))};
  view.strides = {size_t(pixels->stride_y), size_t(pixels->stride_u), size_t(pixels->stride_v)};
  view.timing = timing;
  return view;
}

inline media::FrameView frame_view(std::shared_ptr<const PcmAudioFrame> pcm,
                                   media::FrameTiming timing) {
  media::FrameView view;
  view.owner = pcm;
  view.format =
      pcm->format == PcmFormat::s16 ? media::FrameFormat::pcm_s16 : media::FrameFormat::pcm_f32;
  view.sample_rate = pcm->sample_rate;
  view.channels = pcm->channels;
  view.planes = {pcm->format == PcmFormat::s16
                     ? livekit::compat::as_bytes(livekit::compat::span(pcm->samples))
                     : livekit::compat::as_bytes(livekit::compat::span(pcm->float_samples))};
  view.strides = {size_t(pcm->channels) * (pcm->format == PcmFormat::s16 ? 2 : 4)};
  view.timing = timing;
  return view;
}
}  // namespace livekit::sdk::rtc
