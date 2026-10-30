#include "backends/rtc/control_context.hpp"
#include "livekit/rtc/video_nodes.hpp"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/video_codec.h"
#include "modules/video_coding/include/video_error_codes.h"
#ifdef LIVEKIT_RTC_FFMPEG_CODECS
#include "backends/webrtc/ffmpeg_video_codecs.hpp"
#endif
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <thread>
#include <variant>
#include <utility>

namespace livekit::sdk::rtc::nodes {
namespace {
constexpr uint64_t max_video_timestamp = uint64_t(std::numeric_limits<int64_t>::max() / 100) * 9;

int64_t timestamp_us(uint64_t ticks) {
  return int64_t(ticks / 9 * 100 + ticks % 9 * 100 / 9);
}

webrtc::VideoCodecType native_codec(Codec codec) {
  switch (codec) {
    case Codec::vp8:
      return webrtc::kVideoCodecVP8;
    case Codec::h264:
      return webrtc::kVideoCodecH264;
    case Codec::h265:
      return webrtc::kVideoCodecH265;
    default:
      throw std::invalid_argument("Video codec required");
  }
}

webrtc::SdpVideoFormat native_format(const CodecFormat& format) {
  auto params = format.fmtp;
  if (format.codec == Codec::h264 && params.empty())
    params = {{"packetization-mode", "1"},
              {"profile-level-id", "42e01f"},
              {"level-asymmetry-allowed", "1"}};
  return {format.codec == Codec::vp8    ? "VP8"
          : format.codec == Codec::h264 ? "H264"
                                        : "H265",
          params};
}
}  // namespace

struct VideoCodecNode::Impl final : webrtc::EncodedImageCallback, webrtc::DecodedImageCallback {
  VideoCodecNode& owner;
  const TrackDescriptor descriptor;
  const bool encode;
  const VideoCodecOptions options;

  struct Timing {
    Epoch epoch;
    uint64_t timestamp, generation;
    SteadyClock::time_point captured;
    std::optional<int64_t> sender_capture_time_us;
  };

  struct Job {
    std::variant<VideoFrameEvent, EncodedFrame> frame;
    Timing timing;
    size_t bytes;
  };

  mutable std::mutex mutex;
  std::condition_variable_any changed;
  std::deque<Job> jobs;
  std::map<uint32_t, Timing> pending;
  VideoCodecSnapshot stats;
  std::jthread worker;
  bool active = false, closed = false, needs_keyframe = true;
  Epoch epoch = 0;
  uint64_t generation = 0;
  std::optional<uint64_t> last_timestamp;
  SenderFeedback feedback;
  SteadyClock::time_point input_boundary{};
  std::shared_ptr<webrtc::VideoEncoderFactory> encoder_factory;
  std::shared_ptr<webrtc::VideoDecoderFactory> decoder_factory;
  std::unique_ptr<webrtc::VideoEncoder> encoder;
  std::unique_ptr<webrtc::VideoDecoder> decoder;

  Impl(VideoCodecNode& owner, TrackDescriptor d, bool encode, VideoCodecOptions opts)
      : owner(owner), descriptor(std::move(d)), encode(encode), options(opts) {
    native_codec(descriptor.format.codec);
    if (descriptor.kind != MediaKind::video || descriptor.format.clock_rate != 90000 ||
        descriptor.encodings.size() != 1 || !options.max_frames || options.max_frames > 64 ||
        !options.max_bytes || options.max_bytes > 256 * 1024 * 1024 ||
        options.max_age.count() <= 0 || options.max_age > std::chrono::seconds(5) || !options.fps ||
        options.fps > 240 || !options.max_width || !options.max_height ||
        options.max_width > 8192 || options.max_height > 8192 ||
        (encode && (!descriptor.format.width || !descriptor.format.height)) ||
        descriptor.format.width > options.max_width ||
        descriptor.format.height > options.max_height)
      throw std::invalid_argument("Invalid video codec node configuration");
  }

  void discard_locked() {
    stats.dropped += jobs.size();
    jobs.clear();
    stats.queued_frames = stats.queued_bytes = 0;
    pending.clear();
    ++generation;
    needs_keyframe = true;
  }

  void fail(std::string message) {
    std::exception_ptr error;
    {
      std::lock_guard lock(mutex);
      if (!stats.error.empty()) {
        return;
      }
      stats.error = message;
      error = normalized_failure(std::current_exception()
                                     ? std::current_exception()
                                     : std::make_exception_ptr(std::runtime_error(message)),
                                 {"codec", descriptor.id, owner.getName(), epoch},
                                 ErrorDomain::media, ErrorCode::processing_failed);
      active = false;
      discard_locked();
      changed.notify_all();
    }
    owner.reportFailure(error);
  }

  void recover() {
    {
      std::lock_guard lock(mutex);
      discard_locked();
      ++stats.dropped;
    }
    if (!encode)
      owner.publish(KeyframeRequestEvent{});
  }

  void update(const SenderFeedback& value) {
    std::lock_guard lock(mutex);
    if (value.epoch < epoch ||
        (value.epoch == feedback.epoch && value.revision <= feedback.revision))
      return;
    const auto previous_generation = generation;
    if (value.epoch != epoch) {
      discard_locked();
      epoch = value.epoch;
      last_timestamp.reset();
    }
    if (value.media_generation != feedback.media_generation)
      discard_locked();
    const auto it = value.encodings.find(descriptor.encodings.front().id);
    if (it != value.encodings.end()) {
      auto old = feedback.encodings.find(it->first);
      if (old == feedback.encodings.end() || old->second.active != it->second.active)
        discard_locked();
      if (it->second.keyframe_required &&
          (old == feedback.encodings.end() || !old->second.keyframe_required ||
           old->second.keyframe_requests != it->second.keyframe_requests))
        needs_keyframe = true;
    }
    if (generation != previous_generation)
      input_boundary = SteadyClock::now();
    feedback = value;
  }

  void enqueue(Job job, bool keyframe) {
    bool request = false;
    {
      std::lock_guard lock(mutex);
      if (!active || closed)
        return;
      if (encode) {
        const auto current = feedback.encodings.find(descriptor.encodings.front().id);
        if (current != feedback.encodings.end() && !current->second.active) {
          ++stats.dropped;
          return;
        }
      }
      if (!job.timing.epoch || job.timing.epoch < epoch ||
          job.timing.timestamp > max_video_timestamp || (encode && job.timing.epoch != epoch)) {
        ++stats.dropped;
        return;
      }
      if (job.timing.epoch != epoch) {
        discard_locked();
        epoch = job.timing.epoch;
        last_timestamp.reset();
      }
      const auto now = SteadyClock::now();
      if ((encode && job.timing.captured < input_boundary) ||
          job.timing.captured > now + std::chrono::seconds(1) ||
          now - job.timing.captured > options.max_age ||
          (last_timestamp && job.timing.timestamp <= *last_timestamp)) {
        ++stats.dropped;
        return;
      }
      if (job.bytes > options.max_bytes || jobs.size() >= options.max_frames ||
          stats.queued_bytes > options.max_bytes - job.bytes) {
        discard_locked();
        request = !encode;
      }
      if (job.bytes > options.max_bytes || (!encode && needs_keyframe && !keyframe)) {
        ++stats.dropped;
        request = !encode;
      } else {
        if (!encode && keyframe)
          needs_keyframe = false;
        job.timing.generation = generation;
        last_timestamp = job.timing.timestamp;
        stats.queued_bytes += job.bytes;
        jobs.push_back(std::move(job));
        stats.queued_frames = jobs.size();
        ++stats.admitted;
        changed.notify_all();
      }
    }
    if (request)
      owner.publish(KeyframeRequestEvent{});
  }

  void raw(const VideoFrameEvent& value) {
    if (!value.pixels)
      return;
    const auto& p = *value.pixels;
    if (p.width != descriptor.format.width || p.height != descriptor.format.height) {
      std::lock_guard lock(mutex);
      ++stats.dropped;
      return;
    }
    const int cw = (p.width + 1) / 2, ch = (p.height + 1) / 2;
    const auto valid = [](const auto& data, int stride, int width, int height) {
      return stride >= width && height > 0 && size_t(stride) * (height - 1) + width <= data.size();
    };
    if (!valid(p.y, p.stride_y, p.width, p.height) || !valid(p.u, p.stride_u, cw, ch) ||
        !valid(p.v, p.stride_v, cw, ch) || p.y.size() > options.max_bytes ||
        p.u.size() > options.max_bytes || p.v.size() > options.max_bytes) {
      std::lock_guard lock(mutex);
      ++stats.dropped;
      return;
    }
    enqueue({value,
             {value.epoch, value.timestamp, 0, value.captured},
             p.y.size() + p.u.size() + p.v.size()},
            true);
  }

  void encoded(const EncodedFrame& frame) {
    if (frame.buffer.size())
      enqueue({frame,
               {frame.epoch, frame.timestamp, 0, SteadyClock::now(), frame.sender_capture_time_us},
               frame.buffer.size()},
              frame.keyframe);
  }

  void configure_decoder() {
    webrtc::VideoDecoder::Settings settings;
    settings.set_codec_type(native_codec(descriptor.format.codec));
    settings.set_number_of_cores(1);
    settings.set_max_render_resolution({int(options.max_width), int(options.max_height)});
    settings.set_buffer_pool_size(4);
    if (!decoder->Configure(settings) ||
        decoder->RegisterDecodeCompleteCallback(this) != WEBRTC_VIDEO_CODEC_OK)
      throw std::runtime_error("Video decoder configuration failed");
  }

  void create(const webrtc::Environment& env) {
    if (encode) {
      encoder_factory = webrtc::CreateBuiltinVideoEncoderFactory();
#ifdef LIVEKIT_RTC_FFMPEG_CODECS
      encoder_factory = livekit::sdk::detail::with_ffmpeg_encoders(encoder_factory);
#endif
      encoder = encoder_factory->Create(env, native_format(descriptor.format));
      if (!encoder)
        throw std::runtime_error("Requested video encoder unavailable");
      webrtc::VideoCodec codec;
      codec.codecType = native_codec(descriptor.format.codec);
      codec.width = descriptor.format.width;
      codec.height = descriptor.format.height;
      codec.maxFramerate = options.fps;
      codec.startBitrate = codec.maxBitrate =
          std::max(1u, descriptor.encodings.front().max_bitrate_bps / 1000);
      codec.minBitrate = 1;
      codec.qpMax = 56;
      codec.active = true;
      codec.numberOfSimulcastStreams = 0;
      codec.mode = webrtc::VideoCodecMode::kRealtimeVideo;
      if (codec.codecType == webrtc::kVideoCodecVP8)
        *codec.VP8() = webrtc::VideoEncoder::GetDefaultVp8Settings();
      if (codec.codecType == webrtc::kVideoCodecH264)
        *codec.H264() = webrtc::VideoEncoder::GetDefaultH264Settings();
      if (encoder->InitEncode(&codec, webrtc::VideoEncoder::Settings(
                                          webrtc::VideoEncoder::Capabilities(false), 1, 1200)) !=
              WEBRTC_VIDEO_CODEC_OK ||
          encoder->RegisterEncodeCompleteCallback(this) != WEBRTC_VIDEO_CODEC_OK)
        throw std::runtime_error("Video encoder configuration failed");
    } else {
      decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();
#ifdef LIVEKIT_RTC_FFMPEG_CODECS
      decoder_factory = livekit::sdk::detail::with_ffmpeg_decoders(decoder_factory);
#endif
      decoder = decoder_factory->Create(env, native_format(descriptor.format));
      if (!decoder)
        throw std::runtime_error("Requested video decoder unavailable");
      configure_decoder();
    }
  }

  void process(Job job, Epoch& codec_epoch) {
    uint32_t bitrate = descriptor.encodings.front().max_bitrate_bps;
    bool keyframe = false;
    bool expired = false;
    {
      std::lock_guard lock(mutex);
      if (!active || job.timing.epoch != epoch || job.timing.generation != generation)
        return;
      if (SteadyClock::now() - job.timing.captured > options.max_age || pending.size() >= 32) {
        discard_locked();
        ++stats.dropped;
        expired = true;
      }
      if (encode) {
        auto it = feedback.encodings.find(descriptor.encodings.front().id);
        if (it != feedback.encodings.end()) {
          if (!it->second.active) {
            ++stats.dropped;
            return;
          }
          if (it->second.target_bitrate_bps)
            bitrate = std::min(bitrate, it->second.target_bitrate_bps);
        }
        keyframe = std::exchange(needs_keyframe, false);
      }
    }
    if (expired) {
      if (!encode)
        owner.publish(KeyframeRequestEvent{});
      return;
    }
    if (!encode && codec_epoch != job.timing.epoch) {
      decoder->Release();
      configure_decoder();
      codec_epoch = job.timing.epoch;
    }
    {
      std::lock_guard lock(mutex);
      pending[uint32_t(job.timing.timestamp)] = job.timing;
    }
    int result;
    if (encode) {
      const auto& p = *std::get<VideoFrameEvent>(job.frame).pixels;
      auto pixels = webrtc::I420Buffer::Copy(p.width, p.height, p.y.data(), p.stride_y, p.u.data(),
                                             p.stride_u, p.v.data(), p.stride_v);
      auto frame = webrtc::VideoFrame::Builder()
                       .set_video_frame_buffer(pixels)
                       .set_rtp_timestamp(uint32_t(job.timing.timestamp))
                       .set_timestamp_us(timestamp_us(job.timing.timestamp))
                       .build();
      webrtc::VideoBitrateAllocation allocation;
      allocation.SetBitrate(0, 0, bitrate);
      encoder->SetRates(webrtc::VideoEncoder::RateControlParameters(allocation, options.fps));
      std::vector<webrtc::VideoFrameType> types{keyframe
                                                    ? webrtc::VideoFrameType::kVideoFrameKey
                                                    : webrtc::VideoFrameType::kVideoFrameDelta};
      result = encoder->Encode(frame, &types);
    } else {
      const auto& frame = std::get<EncodedFrame>(job.frame);
      webrtc::EncodedImage image;
      image.SetEncodedData(
          webrtc::EncodedImageBuffer::Create(frame.buffer.bytes().data(), frame.buffer.size()));
      image.SetRtpTimestamp(uint32_t(frame.timestamp));
      image._frameType = frame.keyframe ? webrtc::VideoFrameType::kVideoFrameKey
                                        : webrtc::VideoFrameType::kVideoFrameDelta;
      result = decoder->Decode(image, 0);
    }
    if (result < 0)
      recover();
    else if (result == WEBRTC_VIDEO_CODEC_NO_OUTPUT) {
      std::lock_guard lock(mutex);
      pending.erase(uint32_t(job.timing.timestamp));
    }
  }

  void run(std::stop_token stop, std::promise<void> started) {
    bool ready = false;
    std::optional<webrtc::Environment> env;
    try {
      env.emplace(webrtc::CreateEnvironment());
      create(*env);
      started.set_value();
      ready = true;
      Epoch codec_epoch = 0;
      while (!stop.stop_requested()) {
        std::optional<Job> job;
        {
          std::unique_lock lock(mutex);
          changed.wait(lock, stop, [&] { return !jobs.empty() || !active; });
          if (stop.stop_requested() || !active)
            break;
          job = std::move(jobs.front());
          jobs.pop_front();
          stats.queued_frames = jobs.size();
          stats.queued_bytes -= job->bytes;
        }
        process(std::move(*job), codec_epoch);
      }
    } catch (const std::exception& e) {
      if (!ready)
        started.set_exception(std::current_exception());
      fail(e.what());
    } catch (...) {
      if (!ready)
        started.set_exception(std::current_exception());
      fail("Unknown video codec error");
    }
    if (encoder) {
      encoder->Release();
      encoder.reset();
    }
    if (decoder) {
      decoder->Release();
      decoder.reset();
    }
    encoder_factory.reset();
    decoder_factory.reset();
  }

  std::optional<Timing> completed(uint32_t timestamp) {
    std::unique_lock lock(mutex);
    auto found = pending.find(timestamp);
    if (found == pending.end())
      return {};
    auto timing = found->second;
    pending.erase(found);
    if (!active || timing.epoch != epoch || timing.generation != generation) {
      ++stats.dropped;
      return {};
    }
    if (SteadyClock::now() - timing.captured > options.max_age) {
      ++stats.dropped;
      discard_locked();
      lock.unlock();
      if (!encode)
        owner.publish(KeyframeRequestEvent{});
      return {};
    }
    ++stats.completed;
    return timing;
  }

  Result OnEncodedImage(const webrtc::EncodedImage& image,
                        const webrtc::CodecSpecificInfo*) override {
    try {
      if (auto timing = completed(image.RtpTimestamp())) {
        EncodedFrame frame;
        frame.buffer =
            EncodedBuffer(std::vector<uint8_t>(image.data(), image.data() + image.size()));
        frame.epoch = timing->epoch;
        frame.timestamp = timing->timestamp;
        frame.captured = timing->captured;
        frame.keyframe = image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
        frame.encoding_id = descriptor.encodings.front().id;
        frame.duration = std::chrono::microseconds(1000000 / options.fps);
        owner.publish(EncodedFrameEvent(std::move(frame)));
      }
    } catch (const std::exception& e) {
      fail(e.what());
    } catch (...) {
      fail("Video encode output failed");
    }
    return Result(Result::OK);
  }

  int32_t Decoded(webrtc::VideoFrame& frame) override {
    try {
      if (auto timing = completed(frame.rtp_timestamp())) {
        auto i420 = frame.video_frame_buffer()->ToI420();
        if (!i420 || i420->width() <= 0 || i420->height() <= 0 ||
            uint32_t(i420->width()) > options.max_width ||
            uint32_t(i420->height()) > options.max_height)
          throw std::runtime_error("Decoded dimensions exceed video limits");
        const auto decoded_bytes = size_t(i420->width()) * i420->height() +
                                   2 * size_t((i420->width() + 1) / 2) * ((i420->height() + 1) / 2);
        if (decoded_bytes > options.max_bytes)
          throw std::runtime_error("Decoded pixels exceed video byte limit");
        auto pixels = std::make_shared<livekit::sdk::VideoFrame>();
        pixels->track_id = descriptor.id;
        pixels->width = i420->width();
        pixels->height = i420->height();
        pixels->stride_y = pixels->width;
        pixels->stride_u = pixels->stride_v = (pixels->width + 1) / 2;
        pixels->timestamp_us = timestamp_us(timing->timestamp);
        pixels->stream_generation = timing->epoch;
        const auto copy = [](std::vector<uint8_t>& to, const uint8_t* from, int stride, int width,
                             int height) {
          to.resize(size_t(width) * height);
          for (int y = 0; y < height; ++y)
            std::copy_n(from + size_t(y) * stride, width, to.data() + size_t(y) * width);
        };
        copy(pixels->y, i420->DataY(), i420->StrideY(), pixels->width, pixels->height);
        copy(pixels->u, i420->DataU(), i420->StrideU(), pixels->stride_u, (pixels->height + 1) / 2);
        copy(pixels->v, i420->DataV(), i420->StrideV(), pixels->stride_v, (pixels->height + 1) / 2);
        VideoFrameEvent output;
        output.pixels = std::move(pixels);
        output.timestamp = timing->timestamp;
        output.epoch = timing->epoch;
        output.captured = timing->captured;
        output.sender_capture_time_us = timing->sender_capture_time_us;
        owner.publish(output);
      }
    } catch (const std::exception& e) {
      fail(e.what());
    } catch (...) {
      fail("Video decode output failed");
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }
};

VideoCodecNode::VideoCodecNode(std::string name, TrackDescriptor descriptor, bool encode,
                               VideoCodecOptions options)
    : GraphNode(name, nullptr),
      impl_(std::make_unique<Impl>(*this, std::move(descriptor), encode, options)) {}

VideoCodecNode::~VideoCodecNode() {
  quiesce();
  release();
}

bool VideoCodecNode::doInit() {
  if (impl_->encode) {
    subscribe<VideoFrameEvent>([this](const auto& value) { impl_->raw(value); });
    subscribe<SenderFeedbackEvent>([this](const auto& value) { impl_->update(value.feedback); });
  } else
    subscribe<EncodedFrameEvent>([this](const auto& value) { impl_->encoded(value.frame); });
  return true;
}

bool VideoCodecNode::doStart() {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed)
      return false;
    impl_->active = true;
  }
  std::promise<void> ready;
  auto result = ready.get_future();
  impl_->worker = std::jthread([this, ready = std::move(ready)](std::stop_token stop) mutable {
    detail::CallbackScope worker_scope;
    impl_->run(stop, std::move(ready));
  });
  try {
    result.get();
    return true;
  } catch (...) {
    quiesce();
    throw;
  }
}

void VideoCodecNode::quiesce() {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->active = false;
    impl_->closed = true;
    impl_->discard_locked();
  }
  impl_->worker.request_stop();
  impl_->changed.notify_all();
  if (impl_->worker.joinable())
    impl_->worker.join();
}

void VideoCodecNode::doStop() {
  quiesce();
}

VideoCodecSnapshot VideoCodecNode::snapshot() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}

void VideoCodecNode::throw_if_failed() const {
  if (auto error = failure()) {
    std::rethrow_exception(error);
  }
}
}  // namespace livekit::sdk::rtc::nodes
