#include "backends/ffmpeg/ffmpeg_playback.hpp"

namespace livekit::sdk::detail {
DecoderNode::DecoderNode(const char* name, graphflow::core::EventBus* bus, bool video)
    : MediaNode(name, bus), video_(video) {}

DecoderNode::~DecoderNode() {
  doRelease();
}

bool DecoderNode::doInit() {
  const auto accept = [this](const auto& event) {
    if (event.format) {
      configure(event.format);
      return true;
    }
    if (input_ || input_eos_ || finished_)
      return false;
    input_ = EncodedPacket{event.packet, event.eos};
    return true;
  };
  if (video_)
    subscribe_media<VideoPacket>(accept);
  else
    subscribe_media<AudioPacket>(accept);
  return true;
}

void DecoderNode::configure(std::shared_ptr<const StreamFormat> format) {
  if (format_)
    throw PlayerError("playback/" + getName() + ": duplicate stream format");
  format_ = std::move(format);
  if (!format_->parameters)
    return;  // Unused branch still subscribes and forwards EOS.
  const auto* decoder = avcodec_find_decoder(format_->parameters->codec_id);
  if (!decoder)
    throw PlayerError("playback/" + getName() + ": unsupported codec");
  codec_ = avcodec_alloc_context3(decoder);
  if (!codec_)
    throw std::bad_alloc();
  ffmpeg_check(avcodec_parameters_to_context(codec_, format_->parameters.get()),
               getName() + "/parameters");
  codec_->thread_count = 2;
  ffmpeg_check(avcodec_open2(codec_, decoder, nullptr), getName() + "/open");
  frame_ = av_frame_alloc();
  if (!frame_)
    throw std::bad_alloc();
}

int DecoderNode::stream_index() const {
  return format_ ? format_->stream_index : -1;
}

void DecoderNode::send(const AVPacket* packet) {
  if (!codec_ || !packet || packet->stream_index != stream_index())
    throw PlayerError("playback/" + getName() + ": incompatible packet");
  ffmpeg_check(avcodec_send_packet(codec_, packet), getName() + "/send");
}

void DecoderNode::drain() {
  if (codec_ && !draining_) {
    ffmpeg_check(avcodec_send_packet(codec_, nullptr), getName() + "/drain");
    draining_ = true;
  }
}

std::optional<DecodedFrame> DecoderNode::receive() {
  if (!codec_)
    return {};
  av_frame_unref(frame_);
  const int result = avcodec_receive_frame(codec_, frame_);
  if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
    return {};
  ffmpeg_check(result, getName() + "/receive");
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
  if (!owned)
    throw std::bad_alloc();
  auto ref = std::shared_ptr<const AVFrame>(owned, [](const AVFrame* p) {
    auto* value = const_cast<AVFrame*>(p);
    av_frame_free(&value);
  });
  return DecodedFrame{std::move(ref), pts, duration};
}

void DecoderNode::flush(int64_t position) {
  if (codec_)
    avcodec_flush_buffers(codec_);
  if (frame_)
    av_frame_unref(frame_);
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

void DecoderNode::on_reset(int64_t position) {
  input_.reset();
  pending_.reset();
  receiving_ = input_eos_ = false;
  flush(position);
}

bool DecoderNode::pump() {
  if (finished_)
    return false;
  if (pending_ || (input_eos_ && !receiving_)) {
    bool accepted;
    if (video_) {
      DecodedVideo event;
      event.eos = !pending_;
      if (pending_)
        event.frame = *pending_;
      accepted = publish_media(event);
    } else {
      DecodedAudio event;
      event.eos = !pending_;
      if (pending_)
        event.frame = *pending_;
      accepted = publish_media(event);
    }
    if (accepted) {
      finished_ = !pending_;
      pending_.reset();
    }
    return accepted;
  }
  if (receiving_) {
    pending_ = receive();
    if (!pending_)
      receiving_ = false;
    return true;
  }
  if (!input_)
    return false;
  input_eos_ = input_->eos;
  if (input_eos_)
    drain();
  else
    send(input_->packet.get());
  input_.reset();
  receiving_ = true;
  return true;
}
}  // namespace livekit::sdk::detail
