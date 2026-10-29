#pragma once
#include "livekit/player.hpp"
#include "media_graph/media_flow.hpp"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#include <atomic>
#include <cstdio>

namespace livekit::sdk::detail {
inline constexpr AVRational playback_microseconds{1, 1000000};
void ffmpeg_check(int code, const std::string& operation);
void check_ffmpeg_abi();

// AVIO requests are control messages; file bytes are published along the media
// edge. The demuxer owns AVIO and receives bytes in its subscription callback.
struct FileIORequest : graphflow::core::EventT<FileIORequest> {
  enum Kind { begin, read, seek } kind = begin;

  int size = 0, whence = 0;
  int64_t offset = 0, result = 0;
  bool handled = false;
  std::exception_ptr error;
};

struct SeekableBytes : MediaEvent<SeekableBytes> {
  std::shared_ptr<const std::vector<uint8_t>> bytes;
};

struct EncodedPacket {
  std::shared_ptr<const AVPacket> packet;
  bool eos = false;
};

struct StreamFormat {
  std::shared_ptr<const AVCodecParameters> parameters;
  AVRational time_base{1, 1000000};
  int stream_index = -1;
  int64_t origin = 0, video_duration = 40000;
};

struct VideoPacket : MediaEvent<VideoPacket> {
  std::shared_ptr<const AVPacket> packet;
  std::shared_ptr<const StreamFormat> format;  // preparation event, before packets
};

struct AudioPacket : MediaEvent<AudioPacket> {
  std::shared_ptr<const AVPacket> packet;
  std::shared_ptr<const StreamFormat> format;
};

// AVFrame references own the underlying FFmpeg buffers across event dispatch,
// backpressure and subsequent decoder receives.
struct DecodedFrame {
  std::shared_ptr<const AVFrame> frame;
  int64_t timestamp_us = 0, duration_us = 0;
};

struct DecodedVideo : MediaEvent<DecodedVideo> {
  DecodedFrame frame;
};

struct DecodedAudio : MediaEvent<DecodedAudio> {
  DecodedFrame frame;
};

class FileSourceNode final : public MediaNode {
 public:
  FileSourceNode(graphflow::core::EventBus* bus, std::atomic<bool>& cancel,
                 std::chrono::milliseconds timeout);
  ~FileSourceNode() override;

  void configure(std::string path) {
    path_ = std::move(path);
  }

  const std::string& path() const {
    return path_;
  }

  void begin_io();
  void check_cancelled() const;
  static int interrupt(void* self);

 protected:
  bool doInit() override;
  void doRelease() override;

  void on_reset(int64_t) override {
    finished_ = true;
  }

 private:
  void handle_io(FileIORequest& request);
  std::atomic<bool>& cancel_;
  std::chrono::milliseconds timeout_;
  std::chrono::steady_clock::time_point deadline_{};
  std::string path_;
  FILE* file_ = nullptr;
  int64_t size_ = 0;
};

class DemuxerNode final : public MediaNode {
 public:
  DemuxerNode(graphflow::core::EventBus* bus, std::atomic<bool>& cancel,
              std::chrono::milliseconds timeout);
  ~DemuxerNode() override;

  const PlayerMediaInfo& info() const {
    return info_;
  }

  AVStream* stream(bool video) const;

  int64_t origin() const {
    return origin_;
  }

  int64_t video_duration() const {
    return video_duration_;
  }

  const AVPacket* read();  // nullptr = container EOF
  void seek(int64_t position, int64_t preroll);
  bool pump() override;
  void prepare() override;

 protected:
  bool doInit() override;
  void doRelease() override;
  void on_reset(int64_t position) override;

 private:
  void open_input();
  int64_t request_io(FileIORequest request);
  void begin_io();
  static int interrupt(void* self);
  static int read_bytes(void* self, uint8_t* buffer, int size);
  static int64_t seek_bytes(void* self, int64_t offset, int whence);
  std::string source_topic_;
  std::atomic<bool>& cancel_;
  std::chrono::milliseconds timeout_;
  std::chrono::steady_clock::time_point deadline_{};
  AVIOContext* io_ = nullptr;
  uint8_t* read_target_ = nullptr;
  int read_capacity_ = 0;
  std::exception_ptr io_error_;
  std::shared_ptr<const AVPacket> pending_;
  bool eof_ = false, video_eos_ = false, audio_eos_ = false;
  AVFormatContext* format_ = nullptr;
  AVPacket* packet_ = nullptr;
  int video_ = -1, audio_ = -1;
  int64_t origin_ = 0, video_duration_ = 40000;
  PlayerMediaInfo info_;
};

class DecoderNode : public MediaNode {
 public:
  ~DecoderNode() override;

  bool enabled() const {
    return codec_ != nullptr;
  }

  int stream_index() const;

  int reorder_depth() const {
    return codec_ ? codec_->has_b_frames : 0;
  }

  void send(const AVPacket* packet);
  void drain();
  std::optional<DecodedFrame> receive();
  void flush(int64_t position);
  bool pump() override;

 protected:
  DecoderNode(const char* name, graphflow::core::EventBus* bus, bool video);
  bool doInit() override;
  void doRelease() override;
  void on_reset(int64_t position) override;

 private:
  std::optional<EncodedPacket> input_;
  std::optional<DecodedFrame> pending_;
  bool receiving_ = false, input_eos_ = false;
  void configure(std::shared_ptr<const StreamFormat> format);
  std::shared_ptr<const StreamFormat> format_;
  bool video_, draining_ = false;
  AVCodecContext* codec_ = nullptr;
  AVFrame* frame_ = nullptr;
  int64_t next_time_ = 0;
};

class VideoDecoderNode final : public DecoderNode {
 public:
  explicit VideoDecoderNode(graphflow::core::EventBus* bus)
      : DecoderNode("video.decoder", bus, true) {}
};

class AudioDecoderNode final : public DecoderNode {
 public:
  explicit AudioDecoderNode(graphflow::core::EventBus* bus)
      : DecoderNode("audio.decoder", bus, false) {}
};

class VideoConverterNode final : public MediaNode {
 public:
  explicit VideoConverterNode(graphflow::core::EventBus* bus) : MediaNode("video.rgba", bus) {}

  ~VideoConverterNode() override;
  std::optional<media::VideoFrame> convert(const DecodedFrame& input);
  void flush(int64_t position);
  bool pump() override;

 protected:
  bool doInit() override;
  void doRelease() override;
  void on_reset(int64_t position) override;

 private:
  std::optional<DecodedVideo> input_;
  std::optional<RGBA8> pending_;
  SwsContext* scaler_ = nullptr;
  int64_t discard_before_ = 0;
};

class AudioResamplerNode final : public MediaNode {
 public:
  explicit AudioResamplerNode(graphflow::core::EventBus* bus) : MediaNode("audio.resampler", bus) {}

  ~AudioResamplerNode() override;
  std::optional<media::AudioFrame> convert(const DecodedFrame& input);
  std::optional<media::AudioFrame> drain();
  void flush(int64_t position);
  bool pump() override;

 protected:
  bool doInit() override;
  void doRelease() override;
  void on_reset(int64_t position) override;

 private:
  std::optional<DecodedAudio> input_;
  std::optional<StereoFloat48k> pending_;
  std::optional<media::AudioFrame> emit(std::shared_ptr<std::vector<float>> data, int samples,
                                        int64_t pts);
  SwrContext* resampler_ = nullptr;
  AVChannelLayout layout_{};
  int rate_ = 0, format_ = -1;
  int64_t next_time_ = 0, discard_before_ = 0;
};
}  // namespace livekit::sdk::detail
