#include "backends/ffmpeg/ffmpeg_playback.hpp"
#include <algorithm>
#include <limits>
#include <cstring>

namespace livekit::sdk::detail {
DemuxerNode::DemuxerNode(graphflow::core::EventBus* bus, std::atomic<bool>& cancel,
                         std::chrono::milliseconds timeout)
    : MediaNode("file.demuxer", bus), cancel_(cancel), timeout_(timeout) {}

DemuxerNode::~DemuxerNode() {
  doRelease();
}

Result<int64_t> DemuxerNode::request_io(FileIORequest request) {
  m_eventBus->publish(source_topic_, request);
  if (request.error) {
    return Result<int64_t>::failure(request.error);
  }
  if (!request.handled) {
    return Result<int64_t>::failure(
        playback_failure("playback/file.demuxer: source IO service missing"));
  }
  return Result<int64_t>::success(request.result);
}

Result<void> DemuxerNode::begin_io() {
  deadline_ = std::chrono::steady_clock::now() + timeout_;
  io_error_ = nullptr;
  {
    auto checked = request_io(FileIORequest{});
    if (!checked) {
      return Result<void>::failure(checked.failure_record());
    }
  }
  return Result<void>::success();
}

int DemuxerNode::interrupt(void* self) {
  const auto& node = *static_cast<DemuxerNode*>(self);
  return node.cancel_.load() || std::chrono::steady_clock::now() >= node.deadline_;
}

int DemuxerNode::read_bytes(void* self, uint8_t* buffer, int size) {
  auto& node = *static_cast<DemuxerNode*>(self);
  node.read_target_ = buffer;
  node.read_capacity_ = size;
  FileIORequest request;
  request.kind = FileIORequest::read;
  request.size = size;
  auto requested = node.request_io(request);
  if (!requested) {
    node.io_error_ = requested.failure_record();
  }
  const int result = requested ? static_cast<int>(*requested) : AVERROR_EXIT;
  node.read_target_ = nullptr;
  node.read_capacity_ = 0;
  return result;
}

int64_t DemuxerNode::seek_bytes(void* self, int64_t offset, int whence) {
  auto& node = *static_cast<DemuxerNode*>(self);
  FileIORequest request;
  request.kind = FileIORequest::seek;
  request.offset = offset;
  request.whence = whence;
  auto requested = node.request_io(request);
  if (!requested) {
    node.io_error_ = requested.failure_record();
    return AVERROR_EXIT;
  }
  return *requested;
}

bool DemuxerNode::doInit() {
  auto subscribed =
      subscribe_media<SeekableBytes>([this](const SeekableBytes& event) -> Result<bool> {
        if (!read_target_ || !event.bytes ||
            event.bytes->size() > static_cast<size_t>(read_capacity_)) {
          return Result<bool>::failure(
              playback_failure("playback/file.demuxer: unsolicited source bytes"));
        }
        std::memcpy(read_target_, event.bytes->data(), event.bytes->size());
        return Result<bool>::success(true);
      });
  if (!subscribed) {
    reportError(graph_error(subscribed.failure_record()));
    return false;
  }
  source_topic_ = getSubEdges().front().from() + ".io";
  return true;
}

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
  auto video_sent = publish_media(video, "video");
  if (!video_sent) {
    return Result<void>::failure(video_sent.failure_record());
  }
  auto audio_sent = publish_media(audio, "audio");
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

void DemuxerNode::doRelease() {
  pending_.reset();
  av_packet_free(&packet_);
  avformat_close_input(&format_);
  if (io_) {
    av_freep(&io_->buffer);
    avio_context_free(&io_);
  }
  video_ = audio_ = -1;
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
      accepted = publish_media(event, "video");
    } else {
      AudioPacket event;
      event.packet = pending_;
      accepted = publish_media(event, "audio");
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
      auto published = publish_media(event, "video");
      if (!published) {
        return Result<bool>::failure(published.failure_record());
      }
      progress = video_eos_ = *published;
    }
    if (!audio_eos_) {
      AudioPacket event;
      event.eos = true;
      auto published = publish_media(event, "audio");
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
