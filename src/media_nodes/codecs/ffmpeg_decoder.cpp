#include "backends/ffmpeg/ffmpeg_playback.hpp"

namespace livekit::sdk::detail {
DecoderNode::DecoderNode(const char* name, graphflow::core::EventBus* bus, bool video)
    : MediaNode(name, bus), video_(video) {}

DecoderNode::~DecoderNode() {
  doRelease();
}

bool DecoderNode::doInit() {
  const auto accept = [this](const auto& event) -> Result<bool> {
    if (event.format) {
      {
        auto checked = configure(event.format);
        if (!checked) {
          return Result<bool>::failure(checked.failure_record());
        }
      }
      return Result<bool>::success(true);
    }
    if (input_ || input_eos_ || finished_) {
      return Result<bool>::success(false);
    }
    input_ = EncodedPacket{event.packet, event.eos};
    return Result<bool>::success(true);
  };
  auto subscribed =
      video_ ? subscribe_media<VideoPacket>(accept) : subscribe_media<AudioPacket>(accept);
  if (!subscribed) {
    reportError(graph_error(subscribed.failure_record()));
    return false;
  }
  return true;
}

Result<void> DecoderNode::configure(std::shared_ptr<const StreamFormat> format) {
  if (format_) {
    return Result<void>::failure(
        playback_failure("playback/" + getName() + ": duplicate stream format"));
  }
  format_ = std::move(format);
  if (!format_->parameters) {
    return Result<void>::success();  // Unused branch still subscribes and forwards EOS.
  }
  const auto* decoder = avcodec_find_decoder(format_->parameters->codec_id);
  if (!decoder) {
    return Result<void>::failure(playback_failure("playback/" + getName() + ": unsupported codec"));
  }
  codec_ = avcodec_alloc_context3(decoder);
  if (!codec_) {
    return Result<void>::failure(
        playback_failure("playback: allocation failed", ErrorCode::resource_exhausted));
  }
  {
    auto checked = ffmpeg_check(avcodec_parameters_to_context(codec_, format_->parameters.get()),
                                getName() + "/parameters");
    if (!checked) {
      return Result<void>::failure(checked.failure_record());
    }
  }
  codec_->thread_count = 2;
  {
    auto checked = ffmpeg_check(avcodec_open2(codec_, decoder, nullptr), getName() + "/open");
    if (!checked) {
      return Result<void>::failure(checked.failure_record());
    }
  }
  frame_ = av_frame_alloc();
  if (!frame_) {
    return Result<void>::failure(
        playback_failure("playback: allocation failed", ErrorCode::resource_exhausted));
  }
  return Result<void>::success();
}

int DecoderNode::stream_index() const {
  return format_ ? format_->stream_index : -1;
}

Result<void> DecoderNode::send(const AVPacket* packet) {
  if (!codec_ || !packet || packet->stream_index != stream_index()) {
    return Result<void>::failure(
        playback_failure("playback/" + getName() + ": incompatible packet"));
  }
  {
    auto checked = ffmpeg_check(avcodec_send_packet(codec_, packet), getName() + "/send");
    if (!checked) {
      return Result<void>::failure(checked.failure_record());
    }
  }
  return Result<void>::success();
}

Result<void> DecoderNode::drain() {
  if (codec_ && !draining_) {
    {
      auto checked = ffmpeg_check(avcodec_send_packet(codec_, nullptr), getName() + "/drain");
      if (!checked) {
        return Result<void>::failure(checked.failure_record());
      }
    }
    draining_ = true;
  }
  return Result<void>::success();
}

Result<std::optional<DecodedFrame>> DecoderNode::receive() {
  if (!codec_) {
    return Result<std::optional<DecodedFrame>>::success({});
  }
  av_frame_unref(frame_);
  const int result = avcodec_receive_frame(codec_, frame_);
  if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
    return Result<std::optional<DecodedFrame>>::success({});
  }
  {
    auto checked = ffmpeg_check(result, getName() + "/receive");
    if (!checked) {
      return Result<std::optional<DecodedFrame>>::failure(checked.failure_record());
    }
  }
  const auto time_base = format_->time_base;
  const int64_t pts =
      frame_->best_effort_timestamp == AV_NOPTS_VALUE
          ? next_time_
          : av_rescale_q(frame_->best_effort_timestamp, time_base, playback_microseconds) -
                format_->origin;
  const int64_t duration =
      video_
          ? (frame_->duration > 0 ? av_rescale_q(frame_->duration, time_base, playback_microseconds)
                                  : format_->video_duration)
          : (frame_->sample_rate > 0 ? av_rescale(frame_->nb_samples, 1000000, frame_->sample_rate)
                                     : 0);
  next_time_ = pts + duration;
  auto* owned = av_frame_clone(frame_);
  if (!owned) {
    return Result<std::optional<DecodedFrame>>::failure(
        playback_failure("playback: allocation failed", ErrorCode::resource_exhausted));
  }
  auto ref = std::shared_ptr<const AVFrame>(owned, [](const AVFrame* p) {
    auto* value = const_cast<AVFrame*>(p);
    av_frame_free(&value);
  });
  return Result<std::optional<DecodedFrame>>::success(DecodedFrame{std::move(ref), pts, duration});
}

void DecoderNode::flush(int64_t position) {
  if (codec_) {
    avcodec_flush_buffers(codec_);
  }
  if (frame_) {
    av_frame_unref(frame_);
  }
  draining_ = false;
  next_time_ = position;
}

void DecoderNode::doRelease() {
  input_.reset();
  pending_.reset();
  format_.reset();
  av_frame_free(&frame_);
  avcodec_free_context(&codec_);
}

Result<void> DecoderNode::on_reset(int64_t position) {
  input_.reset();
  pending_.reset();
  receiving_ = input_eos_ = false;
  flush(position);
  return Result<void>::success();
}

Result<bool> DecoderNode::pump() {
  if (finished_) {
    return Result<bool>::success(false);
  }
  if (pending_ || (input_eos_ && !receiving_)) {
    auto accepted = Result<bool>::success(false);
    if (video_) {
      DecodedVideo event;
      event.eos = !pending_;
      if (pending_) {
        event.frame = *pending_;
      }
      accepted = publish_media(event);
    } else {
      DecodedAudio event;
      event.eos = !pending_;
      if (pending_) {
        event.frame = *pending_;
      }
      accepted = publish_media(event);
    }
    if (!accepted) {
      return Result<bool>::failure(accepted.failure_record());
    }
    if (*accepted) {
      finished_ = !pending_;
      pending_.reset();
    }
    return accepted;
  }
  if (receiving_) {
    auto received = receive();
    if (!received) {
      return Result<bool>::failure(received.failure_record());
    }
    pending_ = std::move(*received);
    if (!pending_) {
      receiving_ = false;
    }
    return Result<bool>::success(true);
  }
  if (!input_) {
    return Result<bool>::success(false);
  }
  input_eos_ = input_->eos;
  if (input_eos_) {
    auto checked = drain();
    if (!checked) {
      return Result<bool>::failure(checked.failure_record());
    }
  } else {
    auto checked = send(input_->packet.get());
    if (!checked) {
      return Result<bool>::failure(checked.failure_record());
    }
  }
  input_.reset();
  receiving_ = true;
  return Result<bool>::success(true);
}
}  // namespace livekit::sdk::detail
