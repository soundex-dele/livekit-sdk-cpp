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

int64_t DemuxerNode::request_io(FileIORequest request) {
  m_eventBus->publish(source_topic_, request);
  if (request.error)
    std::rethrow_exception(request.error);
  if (!request.handled)
    throw PlayerError("playback/file.demuxer: source IO service missing");
  return request.result;
}

void DemuxerNode::begin_io() {
  deadline_ = std::chrono::steady_clock::now() + timeout_;
  io_error_ = nullptr;
  request_io(FileIORequest{});
}

int DemuxerNode::interrupt(void* self) {
  const auto& node = *static_cast<DemuxerNode*>(self);
  return node.cancel_.load() || std::chrono::steady_clock::now() >= node.deadline_;
}

int DemuxerNode::read_bytes(void* self, uint8_t* buffer, int size) {
  auto& node = *static_cast<DemuxerNode*>(self);
  node.read_target_ = buffer;
  node.read_capacity_ = size;
  int result;
  try {
    FileIORequest request;
    request.kind = FileIORequest::read;
    request.size = size;
    result = static_cast<int>(node.request_io(request));
  } catch (...) {
    node.io_error_ = std::current_exception();
    result = AVERROR_EXIT;
  }
  node.read_target_ = nullptr;
  node.read_capacity_ = 0;
  return result;
}

int64_t DemuxerNode::seek_bytes(void* self, int64_t offset, int whence) {
  auto& node = *static_cast<DemuxerNode*>(self);
  try {
    FileIORequest request;
    request.kind = FileIORequest::seek;
    request.offset = offset;
    request.whence = whence;
    return node.request_io(request);
  } catch (...) {
    node.io_error_ = std::current_exception();
    return AVERROR_EXIT;
  }
}

bool DemuxerNode::doInit() {
  subscribe_media<SeekableBytes>([this](const SeekableBytes& event) {
    if (!read_target_ || !event.bytes || event.bytes->size() > static_cast<size_t>(read_capacity_))
      throw PlayerError("playback/file.demuxer: unsolicited source bytes");
    std::memcpy(read_target_, event.bytes->data(), event.bytes->size());
    return true;
  });
  source_topic_ = getSubEdges().front().from() + ".io";
  return true;
}

void DemuxerNode::open_input() {
  begin_io();
  auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
  if (!buffer)
    throw std::bad_alloc();
  io_ = avio_alloc_context(buffer, 32768, 0, this, &read_bytes, nullptr, &seek_bytes);
  if (!io_) {
    av_free(buffer);
    throw std::bad_alloc();
  }
  format_ = avformat_alloc_context();
  if (!format_)
    throw std::bad_alloc();
  format_->pb = io_;
  format_->flags |= AVFMT_FLAG_CUSTOM_IO;
  format_->interrupt_callback = {&DemuxerNode::interrupt, this};
  AVDictionary* options = nullptr;
  av_dict_set(&options, "protocol_whitelist", "file", 0);
  const int opened = avformat_open_input(&format_, nullptr, nullptr, &options);
  av_dict_free(&options);
  if (io_error_)
    std::rethrow_exception(io_error_);
  ffmpeg_check(opened, "file.demuxer/open");
  const int probed = avformat_find_stream_info(format_, nullptr);
  if (io_error_)
    std::rethrow_exception(io_error_);
  ffmpeg_check(probed, "file.demuxer/probe");
  origin_ = format_->start_time == AV_NOPTS_VALUE ? 0 : format_->start_time;
  info_.duration_us =
      format_->duration == AV_NOPTS_VALUE ? 0 : std::max<int64_t>(0, format_->duration);
  auto select = [&](AVMediaType type) {
    const int index = av_find_best_stream(format_, type, -1, -1, nullptr, 0);
    if (index == AVERROR_STREAM_NOT_FOUND)
      return -1;
    ffmpeg_check(index, "file.demuxer/select stream");
    return index;
  };
  video_ = select(AVMEDIA_TYPE_VIDEO);
  audio_ = select(AVMEDIA_TYPE_AUDIO);
  if (video_ >= 0 && (format_->streams[video_]->disposition & AV_DISPOSITION_ATTACHED_PIC))
    video_ = -1;
  info_.has_video = video_ >= 0;
  info_.has_audio = audio_ >= 0;
  if (!info_.has_video && !info_.has_audio)
    throw PlayerError("playback/file.demuxer: no audio or video stream");
  if (auto* video = stream(true)) {
    info_.width = video->codecpar->width;
    info_.height = video->codecpar->height;
    auto rate = av_guess_frame_rate(format_, video, nullptr);
    if (rate.num > 0 && rate.den > 0)
      video_duration_ = av_rescale_q(1, av_inv_q(rate), playback_microseconds);
  }
  packet_ = av_packet_alloc();
  if (!packet_)
    throw std::bad_alloc();
}

AVStream* DemuxerNode::stream(bool video) const {
  const int index = video ? video_ : audio_;
  return index >= 0 ? format_->streams[index] : nullptr;
}

void DemuxerNode::prepare() {
  // GraphEngine initializes downstream subscriptions before the file source.
  // Probe only after the complete graph has initialized and its IO service exists.
  open_input();
  const auto describe = [this](bool video) {
    auto result = std::make_shared<StreamFormat>();
    result->origin = origin_;
    result->video_duration = video_duration_;
    if (const auto* selected = stream(video)) {
      auto* parameters = avcodec_parameters_alloc();
      if (!parameters)
        throw std::bad_alloc();
      result->parameters =
          std::shared_ptr<const AVCodecParameters>(parameters, [](const AVCodecParameters* p) {
            auto* value = const_cast<AVCodecParameters*>(p);
            avcodec_parameters_free(&value);
          });
      ffmpeg_check(avcodec_parameters_copy(parameters, selected->codecpar),
                   "file.demuxer/stream format");
      result->time_base = selected->time_base;
      result->stream_index = selected->index;
    }
    return result;
  };
  VideoPacket video;
  video.format = describe(true);
  AudioPacket audio;
  audio.format = describe(false);
  if (!publish_media(video, "video") || !publish_media(audio, "audio"))
    throw PlayerError("playback/file.demuxer: stream format was rejected");
}

const AVPacket* DemuxerNode::read() {
  begin_io();
  av_packet_unref(packet_);
  const int result = av_read_frame(format_, packet_);
  if (io_error_)
    std::rethrow_exception(io_error_);
  if (result == AVERROR_EOF)
    return nullptr;
  ffmpeg_check(result, "file.demuxer/read");
  return packet_;
}

void DemuxerNode::seek(int64_t position, int64_t preroll) {
  begin_io();
  // The first keyframe DTS can precede the container's first presentation PTS
  // (notably MPEG-TS with B frames). Allow preroll before normalized zero.
  const int64_t latest = position + origin_;
  const int64_t target = latest - preroll;
  const int result =
      avformat_seek_file(format_, -1, std::numeric_limits<int64_t>::min(), target, latest, 0);
  if (io_error_)
    std::rethrow_exception(io_error_);
  ffmpeg_check(result, "file.demuxer/seek");
  av_packet_unref(packet_);
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

void DemuxerNode::on_reset(int64_t position) {
  pending_.reset();
  eof_ = video_eos_ = audio_eos_ = false;
  const auto* video = stream(true);
  const int64_t preroll =
      video ? std::max<int64_t>(1000000, video_duration_ * (video->codecpar->video_delay + 1)) : 0;
  seek(position, preroll);
}

bool DemuxerNode::pump() {
  if (finished_)
    return false;
  if (pending_) {
    bool accepted;
    if (pending_->stream_index == video_) {
      VideoPacket event;
      event.packet = pending_;
      accepted = publish_media(event, "video");
    } else {
      AudioPacket event;
      event.packet = pending_;
      accepted = publish_media(event, "audio");
    }
    if (accepted)
      pending_.reset();
    return accepted;
  }
  if (eof_) {
    bool progress = false;
    if (!video_eos_) {
      VideoPacket event;
      event.eos = true;
      progress = video_eos_ = publish_media(event, "video");
    }
    if (!audio_eos_) {
      AudioPacket event;
      event.eos = true;
      audio_eos_ = publish_media(event, "audio");
      progress |= audio_eos_;
    }
    finished_ = video_eos_ && audio_eos_;
    return progress;
  }
  const auto* packet = read();
  if (!packet) {
    eof_ = true;
    return true;
  }
  if (packet->stream_index == video_ || packet->stream_index == audio_) {
    if (packet->size > 64 * 1024 * 1024)
      throw PlayerError("playback/demux: packet exceeds 64 MiB limit");
    auto* owned = av_packet_clone(packet);
    if (!owned)
      throw std::bad_alloc();
    pending_ = std::shared_ptr<const AVPacket>(owned, [](const AVPacket* p) {
      auto* value = const_cast<AVPacket*>(p);
      av_packet_free(&value);
    });
  }
  return true;
}
}  // namespace livekit::sdk::detail
