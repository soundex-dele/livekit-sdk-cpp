#include "backends/ffmpeg/ffmpeg_playback.hpp"
#include <algorithm>
#include <limits>

namespace livekit::sdk::detail {
Result<void> DemuxerNode::open_input() {
  {
    auto checked = begin_io();
    if (!checked) {
      return Result<void>::failure(checked.failure_record());
    }
  }
  auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
  if (!buffer) {
    return Result<void>::failure(
        playback_failure("playback: allocation failed", ErrorCode::resource_exhausted));
  }
  io_ = avio_alloc_context(buffer, 32768, 0, this, &read_bytes, nullptr, &seek_bytes);
  if (!io_) {
    av_free(buffer);
    return Result<void>::failure(
        playback_failure("playback: allocation failed", ErrorCode::resource_exhausted));
  }
  format_ = avformat_alloc_context();
  if (!format_) {
    return Result<void>::failure(
        playback_failure("playback: allocation failed", ErrorCode::resource_exhausted));
  }
  format_->pb = io_;
  format_->flags |= AVFMT_FLAG_CUSTOM_IO;
  format_->interrupt_callback = {&DemuxerNode::interrupt, this};
  AVDictionary* options = nullptr;
  av_dict_set(&options, "protocol_whitelist", "file", 0);
  const int opened = avformat_open_input(&format_, nullptr, nullptr, &options);
  av_dict_free(&options);
  if (io_error_) {
    return Result<void>::failure(io_error_);
  }
  {
    auto checked = ffmpeg_check(opened, "file.demuxer/open");
    if (!checked) {
      return Result<void>::failure(checked.failure_record());
    }
  }
  const int probed = avformat_find_stream_info(format_, nullptr);
  if (io_error_) {
    return Result<void>::failure(io_error_);
  }
  {
    auto checked = ffmpeg_check(probed, "file.demuxer/probe");
    if (!checked) {
      return Result<void>::failure(checked.failure_record());
    }
  }
  origin_ = format_->start_time == AV_NOPTS_VALUE ? 0 : format_->start_time;
  info_.duration_us =
      format_->duration == AV_NOPTS_VALUE ? 0 : std::max<int64_t>(0, format_->duration);
  video_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  audio_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (video_ < 0 && video_ != AVERROR_STREAM_NOT_FOUND) {
    {
      auto checked = ffmpeg_check(video_, "file.demuxer/select video");
      if (!checked) {
        return Result<void>::failure(checked.failure_record());
      }
    }
  }
  if (audio_ < 0 && audio_ != AVERROR_STREAM_NOT_FOUND) {
    {
      auto checked = ffmpeg_check(audio_, "file.demuxer/select audio");
      if (!checked) {
        return Result<void>::failure(checked.failure_record());
      }
    }
  }
  if (video_ >= 0 && (format_->streams[video_]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
    video_ = -1;
  }
  info_.has_video = video_ >= 0;
  info_.has_audio = audio_ >= 0;
  if (!info_.has_video && !info_.has_audio) {
    return Result<void>::failure(
        playback_failure("playback/file.demuxer: no audio or video stream"));
  }
  if (auto* video = stream(true)) {
    info_.width = video->codecpar->width;
    info_.height = video->codecpar->height;
    auto rate = av_guess_frame_rate(format_, video, nullptr);
    if (rate.num > 0 && rate.den > 0) {
      video_duration_ = av_rescale_q(1, av_inv_q(rate), playback_microseconds);
    }
  }
  packet_ = av_packet_alloc();
  if (!packet_) {
    return Result<void>::failure(
        playback_failure("playback: allocation failed", ErrorCode::resource_exhausted));
  }
  return Result<void>::success();
}

AVStream* DemuxerNode::stream(bool video) const {
  const int index = video ? video_ : audio_;
  return index >= 0 ? format_->streams[index] : nullptr;
}

Result<void> DemuxerNode::prepare() {
  auto opened = open_input();
  if (!opened) {
    return opened;
  }
  const auto describe = [this](bool video) -> Result<std::shared_ptr<const StreamFormat>> {
    auto result = std::make_shared<StreamFormat>();
    result->origin = origin_;
    result->video_duration = video_duration_;
    if (const auto* selected = stream(video)) {
      auto* parameters = avcodec_parameters_alloc();
      if (!parameters) {
        return Result<std::shared_ptr<const StreamFormat>>::failure(
            playback_failure("playback: allocation failed", ErrorCode::resource_exhausted));
      }
      result->parameters =
          std::shared_ptr<const AVCodecParameters>(parameters, [](const AVCodecParameters* p) {
            auto* value = const_cast<AVCodecParameters*>(p);
            avcodec_parameters_free(&value);
          });
      auto copied = ffmpeg_check(avcodec_parameters_copy(parameters, selected->codecpar),
                                 "file.demuxer/stream format");
      if (!copied) {
        return Result<std::shared_ptr<const StreamFormat>>::failure(copied.failure_record());
      }
      result->time_base = selected->time_base;
      result->stream_index = selected->index;
    }
    return Result<std::shared_ptr<const StreamFormat>>::success(std::move(result));
  };
  auto video_format = describe(true);
  if (!video_format) {
    return Result<void>::failure(video_format.failure_record());
  }
  auto audio_format = describe(false);
  if (!audio_format) {
    return Result<void>::failure(audio_format.failure_record());
  }
  VideoPacket video;
  video.format = std::move(*video_format);
  AudioPacket audio;
  audio.format = std::move(*audio_format);
  auto video_sent = publish_media(video_out_, video);
  if (!video_sent) {
    return Result<void>::failure(video_sent.failure_record());
  }
  auto audio_sent = publish_media(audio_out_, audio);
  if (!audio_sent) {
    return Result<void>::failure(audio_sent.failure_record());
  }
  if (!*video_sent || !*audio_sent) {
    return Result<void>::failure(
        playback_failure("playback/file.demuxer: stream format was rejected"));
  }
  return Result<void>::success();
}

Result<const AVPacket*> DemuxerNode::read() {
  {
    auto checked = begin_io();
    if (!checked) {
      return Result<const AVPacket*>::failure(checked.failure_record());
    }
  }
  av_packet_unref(packet_);
  const int result = av_read_frame(format_, packet_);
  if (io_error_) {
    return Result<const AVPacket*>::failure(io_error_);
  }
  if (result == AVERROR_EOF) {
    return Result<const AVPacket*>::success(nullptr);
  }
  {
    auto checked = ffmpeg_check(result, "file.demuxer/read");
    if (!checked) {
      return Result<const AVPacket*>::failure(checked.failure_record());
    }
  }
  return Result<const AVPacket*>::success(packet_);
}

Result<void> DemuxerNode::seek(int64_t position, int64_t preroll) {
  {
    auto checked = begin_io();
    if (!checked) {
      return Result<void>::failure(checked.failure_record());
    }
  }
  // A seek command can interrupt the previous AVIO read. FFmpeg retains its
  // error on the AVIOContext, even after a successful buffered seek. Only
  // clear that superseded interrupt, not genuine read errors or normal EOF.
  if (io_->error == AVERROR_EXIT) {
    io_->error = 0;
    io_->eof_reached = 0;
  }
  // The first keyframe DTS can precede the container's first presentation PTS
  // (notably MPEG-TS with B frames). Allow preroll before normalized zero.
  const int64_t latest = position + origin_;
  const int64_t target = latest - preroll;
  const int result =
      avformat_seek_file(format_, -1, std::numeric_limits<int64_t>::min(), target, latest, 0);
  if (io_error_) {
    return Result<void>::failure(io_error_);
  }
  {
    auto checked = ffmpeg_check(result, "file.demuxer/seek");
    if (!checked) {
      return Result<void>::failure(checked.failure_record());
    }
  }
  av_packet_unref(packet_);
  return Result<void>::success();
}

Result<void> DemuxerNode::on_reset(int64_t position) {
  pending_.reset();
  eof_ = video_eos_ = audio_eos_ = false;
  const auto* video = stream(true);
  const int64_t preroll =
      video ? std::max<int64_t>(1000000, video_duration_ * (video->codecpar->video_delay + 1)) : 0;
  return seek(position, preroll);
}

Result<bool> DemuxerNode::pump() {
  if (finished_) {
    return Result<bool>::success(false);
  }
  if (pending_) {
    auto accepted = Result<bool>::success(false);
    if (pending_->stream_index == video_) {
      VideoPacket event;
      event.packet = pending_;
      accepted = publish_media(video_out_, event);
    } else {
      AudioPacket event;
      event.packet = pending_;
      accepted = publish_media(audio_out_, event);
    }
    if (!accepted) {
      return Result<bool>::failure(accepted.failure_record());
    }
    if (*accepted) {
      pending_.reset();
    }
    return accepted;
  }
  if (eof_) {
    bool progress = false;
    if (!video_eos_) {
      VideoPacket event;
      event.eos = true;
      auto published = publish_media(video_out_, event);
      if (!published) {
        return Result<bool>::failure(published.failure_record());
      }
      progress = video_eos_ = *published;
    }
    if (!audio_eos_) {
      AudioPacket event;
      event.eos = true;
      auto published = publish_media(audio_out_, event);
      if (!published) {
        return Result<bool>::failure(published.failure_record());
      }
      audio_eos_ = *published;
      progress |= audio_eos_;
    }
    finished_ = video_eos_ && audio_eos_;
    return Result<bool>::success(progress);
  }
  auto read_result = read();
  if (!read_result) {
    return Result<bool>::failure(read_result.failure_record());
  }
  const auto* packet = *read_result;
  if (!packet) {
    eof_ = true;
    return Result<bool>::success(true);
  }
  if (packet->stream_index == video_ || packet->stream_index == audio_) {
    if (packet->size > 64 * 1024 * 1024) {
      return Result<bool>::failure(playback_failure("playback/demux: packet exceeds 64 MiB limit"));
    }
    auto* owned = av_packet_clone(packet);
    if (!owned) {
      return Result<bool>::failure(
          playback_failure("playback: allocation failed", ErrorCode::resource_exhausted));
    }
    pending_ = std::shared_ptr<const AVPacket>(owned, [](const AVPacket* p) {
      auto* value = const_cast<AVPacket*>(p);
      av_packet_free(&value);
    });
  }
  return Result<bool>::success(true);
}
}  // namespace livekit::sdk::detail
