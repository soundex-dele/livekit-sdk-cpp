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
inline Failure playback_failure(std::string message,
                                ErrorCode code = ErrorCode::processing_failed) {
  return make_failure(ErrorDomain::media, code, std::move(message));
}

inline constexpr AVRational playback_microseconds{1, 1000000};
Result<void> ffmpeg_check(int code, const std::string& operation);
Result<void> check_ffmpeg_abi();

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

struct VideoPacket : MediaEvent {
  static constexpr const char* name = "VideoPacket";
  std::shared_ptr<const AVPacket> packet;
  std::shared_ptr<const StreamFormat> format;  // preparation event, before packets
};

struct AudioPacket : MediaEvent {
  static constexpr const char* name = "AudioPacket";
  std::shared_ptr<const AVPacket> packet;
  std::shared_ptr<const StreamFormat> format;
};

// AVFrame references own the underlying FFmpeg buffers across event dispatch,
// backpressure and subsequent decoder receives.
struct DecodedFrame {
  std::shared_ptr<const AVFrame> frame;
  int64_t timestamp_us = 0, duration_us = 0;
};

struct DecodedVideo : MediaEvent {
  static constexpr const char* name = "DecodedVideo";
  DecodedFrame frame;
};

struct DecodedAudio : MediaEvent {
  static constexpr const char* name = "DecodedAudio";
  DecodedFrame frame;
};

class DemuxerNode final : public MediaNode {
 public:
  DemuxerNode(std::atomic<bool>& cancel, std::chrono::milliseconds timeout);
  ~DemuxerNode() override;

  void configure(std::string path) {
    path_ = std::move(path);
  }

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

  Result<const AVPacket*> read();  // nullptr = container EOF
  Result<void> seek(int64_t position, int64_t preroll);
  Result<bool> pump() override;
  Result<void> prepare() override;

 protected:
  void bind(graphflow::Binder& b) override {
    video_out_ = b.output(graphflow::Out<VideoPacket>{"video", graphflow::Connections::ExactlyOne});
    audio_out_ = b.output(graphflow::Out<AudioPacket>{"audio", graphflow::Connections::ExactlyOne});
  }

  Result<void> doInit() override;
  void doRelease() override;
  Result<void> on_reset(int64_t position) override;

 private:
  graphflow::Emitter<VideoPacket> video_out_;
  graphflow::Emitter<AudioPacket> audio_out_;
  Result<void> open_input();
  Result<void> check_cancelled() const;
  Result<void> begin_io();
  static int interrupt(void* self);
  static int read_bytes(void* self, uint8_t* buffer, int size);
  static int64_t seek_bytes(void* self, int64_t offset, int whence);
  std::string path_;
  struct PlaybackFile;
  std::unique_ptr<PlaybackFile> file_;
  int64_t size_ = 0;
  std::atomic<bool>& cancel_;
  std::chrono::milliseconds timeout_;
  std::chrono::steady_clock::time_point deadline_{};
  AVIOContext* io_ = nullptr;
  Failure io_error_;
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

  Result<void> send(const AVPacket* packet);
  Result<void> drain();
  Result<std::optional<DecodedFrame>> receive();
  void flush(int64_t position);
  Result<bool> pump() override;

 protected:
  DecoderNode(const char* name, bool video);
  void bind(graphflow::Binder& b) override;
  void doRelease() override;
  Result<void> on_reset(int64_t position) override;

 private:
  graphflow::Emitter<DecodedVideo> video_out_;
  graphflow::Emitter<DecodedAudio> audio_out_;
  std::optional<EncodedPacket> input_;
  std::optional<DecodedFrame> pending_;
  bool receiving_ = false, input_eos_ = false;
  Result<void> configure(std::shared_ptr<const StreamFormat> format);
  std::shared_ptr<const StreamFormat> format_;
  bool video_, draining_ = false;
  AVCodecContext* codec_ = nullptr;
  AVFrame* frame_ = nullptr;
  int64_t next_time_ = 0;
};

class VideoDecoderNode final : public DecoderNode {
 public:
  VideoDecoderNode() : DecoderNode("video.decoder", true) {}
};

class AudioDecoderNode final : public DecoderNode {
 public:
  AudioDecoderNode() : DecoderNode("audio.decoder", false) {}
};

class VideoConverterNode final : public MediaNode {
 public:
  VideoConverterNode() : MediaNode("video.rgba") {}

  ~VideoConverterNode() override;
  Result<std::optional<media::VideoFrame>> convert(const DecodedFrame& input);
  void flush(int64_t position);
  Result<bool> pump() override;

 protected:
  void bind(graphflow::Binder& b) override;
  void doRelease() override;
  Result<void> on_reset(int64_t position) override;

 private:
  graphflow::Emitter<RGBA8> output_;
  std::optional<DecodedVideo> input_;
  std::optional<RGBA8> pending_;
  SwsContext* scaler_ = nullptr;
  int64_t discard_before_ = 0;
};

class AudioResamplerNode final : public MediaNode {
 public:
  AudioResamplerNode() : MediaNode("audio.resampler") {}

  ~AudioResamplerNode() override;
  Result<std::optional<media::AudioFrame>> convert(const DecodedFrame& input);
  Result<std::optional<media::AudioFrame>> drain();
  void flush(int64_t position);
  Result<bool> pump() override;

 protected:
  void bind(graphflow::Binder& b) override;
  void doRelease() override;
  Result<void> on_reset(int64_t position) override;

 private:
  graphflow::Emitter<StereoFloat48k> output_;
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
