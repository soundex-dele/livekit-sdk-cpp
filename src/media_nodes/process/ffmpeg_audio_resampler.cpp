#include "backends/ffmpeg/ffmpeg_playback.hpp"
#include <algorithm>

namespace livekit::sdk::detail {
AudioResamplerNode::~AudioResamplerNode() {
  doRelease();
}

bool AudioResamplerNode::doInit() {
  auto subscribed =
      subscribe_media<DecodedAudio>([this](const DecodedAudio& event) -> Result<bool> {
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

void AudioResamplerNode::doRelease() {
  input_.reset();
  pending_.reset();
  swr_free(&resampler_);
  av_channel_layout_uninit(&layout_);
  rate_ = 0;
  format_ = -1;
}

void AudioResamplerNode::flush(int64_t position) {
  doRelease();
  next_time_ = discard_before_ = position;
}

std::optional<media::AudioFrame> AudioResamplerNode::emit(std::shared_ptr<std::vector<float>> data,
                                                          int samples, int64_t pts) {
  next_time_ = pts + av_rescale(samples, 1000000, 48000);
  if (samples == 0 || next_time_ <= discard_before_) {
    return {};
  }
  int skip = 0;
  if (pts < discard_before_) {
    skip = static_cast<int>(std::min<int64_t>(
        samples, av_rescale_rnd(discard_before_ - pts, 48000, 1000000, AV_ROUND_UP)));
  }
  if (skip == samples) {
    return {};
  }
  if (skip) {
    data->erase(data->begin(), data->begin() + skip * 2);
  }
  data->resize(static_cast<size_t>(samples - skip) * 2);
  media::AudioFrame result;
  result.samples = samples - skip;
  result.timestamp_us = pts + av_rescale(skip, 1000000, 48000);
  result.duration_us = av_rescale(result.samples, 1000000, 48000);
  result.pcm = std::move(data);
  return result;
}

Result<std::optional<media::AudioFrame>> AudioResamplerNode::drain() {
  if (!resampler_) {
    return Result<std::optional<media::AudioFrame>>::success({});
  }
  const int capacity = std::max(256, swr_get_out_samples(resampler_, 0));
  auto data = std::make_shared<std::vector<float>>(static_cast<size_t>(capacity) * 2);
  uint8_t* planes[] = {reinterpret_cast<uint8_t*>(data->data())};
  const int samples = swr_convert(resampler_, planes, capacity, nullptr, 0);
  {
    auto checked = ffmpeg_check(samples, "audio.resampler/drain");
    if (!checked) {
      return Result<std::optional<media::AudioFrame>>::failure(checked.failure_record());
    }
  }
  return Result<std::optional<media::AudioFrame>>::success(
      emit(std::move(data), samples, next_time_));
}

Result<std::optional<media::AudioFrame>> AudioResamplerNode::convert(const DecodedFrame& input) {
  const auto* frame = input.frame.get();
  if (!frame || frame->sample_rate <= 0 || frame->nb_samples <= 0 || frame->nb_samples > 1048576) {
    return Result<std::optional<media::AudioFrame>>::failure(
        playback_failure("playback/audio.resampler: invalid audio frame"));
  }
  if (!resampler_ || rate_ != frame->sample_rate || format_ != frame->format ||
      av_channel_layout_compare(&layout_, &frame->ch_layout) != 0) {
    if (resampler_) {
      return Result<std::optional<media::AudioFrame>>::failure(playback_failure(
          "playback/audio.resampler: mid-stream format change requires reopening"));
    }
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    // FFmpeg takes mutable layouts but does not modify the input layout here.
    AVChannelLayout source_layout{};
    {
      auto checked = ffmpeg_check(av_channel_layout_copy(&source_layout, &frame->ch_layout),
                                  "audio.resampler/layout");
      if (!checked) {
        return Result<std::optional<media::AudioFrame>>::failure(checked.failure_record());
      }
    }
    const int result = swr_alloc_set_opts2(
        &resampler_, &stereo, AV_SAMPLE_FMT_FLT, 48000, &source_layout,
        static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&source_layout);
    {
      auto checked = ffmpeg_check(result, "audio.resampler/options");
      if (!checked) {
        return Result<std::optional<media::AudioFrame>>::failure(checked.failure_record());
      }
    }
    {
      auto checked = ffmpeg_check(swr_init(resampler_), "audio.resampler/init");
      if (!checked) {
        return Result<std::optional<media::AudioFrame>>::failure(checked.failure_record());
      }
    }
    rate_ = frame->sample_rate;
    format_ = frame->format;
    {
      auto checked = ffmpeg_check(av_channel_layout_copy(&layout_, &frame->ch_layout),
                                  "audio.resampler/layout");
      if (!checked) {
        return Result<std::optional<media::AudioFrame>>::failure(checked.failure_record());
      }
    }
  }
  const int64_t delay = swr_get_delay(resampler_, frame->sample_rate);
  const int capacity = swr_get_out_samples(resampler_, frame->nb_samples);
  {
    auto checked = ffmpeg_check(capacity, "audio.resampler/capacity");
    if (!checked) {
      return Result<std::optional<media::AudioFrame>>::failure(checked.failure_record());
    }
  }
  auto data = std::make_shared<std::vector<float>>(static_cast<size_t>(capacity) * 2);
  uint8_t* planes[] = {reinterpret_cast<uint8_t*>(data->data())};
  const int samples =
      swr_convert(resampler_, planes, capacity, const_cast<const uint8_t**>(frame->extended_data),
                  frame->nb_samples);
  {
    auto checked = ffmpeg_check(samples, "audio.resampler/convert");
    if (!checked) {
      return Result<std::optional<media::AudioFrame>>::failure(checked.failure_record());
    }
  }
  return Result<std::optional<media::AudioFrame>>::success(
      emit(std::move(data), samples,
           input.timestamp_us - av_rescale(delay, 1000000, frame->sample_rate)));
}

Result<void> AudioResamplerNode::on_reset(int64_t position) {
  flush(position);
  return Result<void>::success();
}

Result<bool> AudioResamplerNode::pump() {
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
  StereoFloat48k event;
  if (input_->eos) {
    // Keep EOS in the input slot until all resampler tail samples are published.
    auto drained = drain();
    if (!drained) {
      return Result<bool>::failure(drained.failure_record());
    }
    if (auto tail = std::move(*drained)) {
      event.frame = std::move(*tail);
      pending_ = std::move(event);
    } else {
      event.eos = true;
      pending_ = event;
      input_.reset();
    }
  } else {
    auto converted = convert(input_->frame);
    if (!converted) {
      return Result<bool>::failure(converted.failure_record());
    }
    if (auto frame = std::move(*converted)) {
      event.frame = std::move(*frame);
      pending_ = std::move(event);
    }
    input_.reset();
  }
  return Result<bool>::success(true);
}
}  // namespace livekit::sdk::detail
