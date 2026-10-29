#include "livekit/rtc/input_node.hpp"
#include "media_graph/raw_frame_queue.hpp"
#include "media_nodes/capture/audio_processor.hpp"
#include "media_nodes/capture/audio_source.hpp"
#include "media_nodes/capture/video_source.hpp"
#include "media_nodes/capture/encoded_source.hpp"
#include "media_nodes/capture/capture.hpp"
#include "support/process.hpp"
#include "api/video/i420_buffer.h"
#include "rtc_base/time_utils.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <numbers>
#include <thread>

namespace livekit::sdk::rtc::nodes {
namespace {
using namespace std::chrono_literals;

MediaInputOptions resolve_microphone(MediaInputOptions options) {
  if (options.microphone) {
    if (options.audio_source)
      throw std::invalid_argument("Choose microphone or application audio source");
    options.audio_source = options.microphone->source();
  }
  return options;
}

std::string codec_name(Codec c) {
  switch (c) {
    case Codec::opus:
      return "opus";
    case Codec::vp8:
      return "vp8";
    case Codec::h264:
      return "h264";
    case Codec::h265:
      return "h265";
  }
  throw std::invalid_argument("Invalid input codec");
}

std::vector<std::string> input_args(const std::string& input) {
  for (const auto& codec : {"h264", "h265", "vp8", "opus"}) {
    const auto prefix = std::string(codec) + "://";
    if (!input.starts_with(prefix))
      continue;
    const auto address = input.substr(prefix.size());
    return {"-probesize",
            "32768",
            "-analyzeduration",
            "0",
            "-f",
            std::string(codec) == "h265"   ? "hevc"
            : std::string(codec) == "vp8"  ? "ivf"
            : std::string(codec) == "opus" ? "ogg"
                                           : "h264",
            "-i",
            address.starts_with('/') ? "unix:" + address : "tcp://" + address};
  }
  return {"-i", input};
}
}  // namespace

struct MediaInputNode::Impl {
  MediaInputNode& owner;
  const TrackDescriptor descriptor;
  const MediaInputOptions options;
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  MediaInputSnapshot stats;
  SenderFeedback feedback;
  SteadyClock::time_point boundary{}, origin = SteadyClock::now();
  uint64_t generation = 0;
  bool started = false, closed = false, microphone_reporter = false;
  std::atomic<bool> stop{false};
  std::atomic<bool> capture_stop{false};
  std::jthread worker;
  std::shared_ptr<lk::Process> process;
  std::unique_ptr<livekit::sdk::detail::I420SourceReader> video;
  std::unique_ptr<livekit::sdk::detail::PcmSourceReader> audio;
  std::unique_ptr<livekit::sdk::detail::EncodedSourceReader> encoded;
  std::unique_ptr<detail::AudioProcessor> audio_processor;
  std::optional<uint64_t> audio_generation;  // Input worker only.

  struct Token {
    Epoch epoch;
    uint64_t generation;
  };

  Impl(MediaInputNode& owner, TrackDescriptor d, MediaInputOptions opts)
      : owner(owner), descriptor(std::move(d)), options(resolve_microphone(std::move(opts))) {
    const bool is_video = descriptor.kind == MediaKind::video;
    const int inputs = int(options.generated) + int(!options.input.empty()) +
                       int(options.capture != CaptureKind::input) +
                       int(bool(options.video_source)) + int(bool(options.audio_source)) +
                       int(bool(options.encoded_source)) + int(bool(options.shared_frames));
    if (inputs != 1 || !options.fps || options.fps > 240 || options.ffmpeg.empty() ||
        descriptor.encodings.size() != 1 ||
        (descriptor.kind != MediaKind::video && descriptor.kind != MediaKind::audio) ||
        descriptor.format.clock_rate != (is_video ? 90000u : 48000u) ||
        (is_video ? descriptor.format.codec == Codec::opus
                  : descriptor.format.codec != Codec::opus) ||
        (is_video && (!descriptor.format.width || !descriptor.format.height ||
                      descriptor.format.width > 8192 || descriptor.format.height > 8192)) ||
        (!is_video && (descriptor.format.channels < 1 || descriptor.format.channels > 2)) ||
        (options.capture != CaptureKind::input && options.capture != CaptureKind::camera &&
         options.capture != CaptureKind::screen) ||
        (options.capture != CaptureKind::input && !is_video) || options.device_index < 0 ||
        (options.loop && options.input.empty()))
      throw std::invalid_argument("Invalid media input configuration");
    codec_name(descriptor.format.codec);
    if (options.audio_processing && options.audio_processing->enabled) {
      if (is_video || options.encoded_source || options.shared_frames) {
        throw std::invalid_argument("APM belongs on the raw capture before shared fanout");
      }
      audio_processor = std::make_unique<detail::AudioProcessor>(*options.audio_processing,
                                                                 options.echo_reference);
    }
    if (descriptor.follow_subscriber_demand && (!is_video || !options.input.empty()))
      throw std::invalid_argument("Subscriber demand requires a live video source");
    if (options.video_source) {
      const auto format = options.video_source->options();
      if (!is_video || format.dynamic_dimensions || format.width != descriptor.format.width ||
          format.height != descriptor.format.height)
        throw std::invalid_argument("Application video dimensions must match the published format");
      video = std::make_unique<livekit::sdk::detail::I420SourceReader>(options.video_source);
    }
    if (options.audio_source) {
      if (is_video || options.audio_source->options().channels != descriptor.format.channels)
        throw std::invalid_argument("Application audio channels must match the published format");
      audio = std::make_unique<livekit::sdk::detail::PcmSourceReader>(options.audio_source);
    }
    if (options.encoded_source) {
      const auto format = options.encoded_source->options();
      if (format.codec != codec_name(descriptor.format.codec) ||
          (is_video &&
           (format.width != descriptor.format.width || format.height != descriptor.format.height)))
        throw std::invalid_argument("Application encoded format must match the published format");
      encoded = std::make_unique<livekit::sdk::detail::EncodedSourceReader>(options.encoded_source);
    }
  }

  void start_readers() {
    if (options.shared_frames) {
      options.shared_frames->start();
    }
    if (video)
      video->start();
    if (audio)
      audio->start();
    if (encoded)
      encoded->start();
  }

  void stop_readers() {
    if (options.shared_frames) {
      options.shared_frames->stop();
    }
    if (video)
      video->stop();
    if (audio)
      audio->stop();
    if (encoded)
      encoded->stop();
  }

  bool enabled(const SenderFeedback& value) const {
    if (!descriptor.follow_subscriber_demand)
      return true;
    const auto encoding = value.encodings.find(descriptor.encodings.front().id);
    return value.epoch && encoding != value.encodings.end() && encoding->second.active;
  }

  void update(const SenderFeedback& value) {
    std::lock_guard lock(mutex);
    if (closed || value.epoch < feedback.epoch ||
        (value.epoch == feedback.epoch && value.revision <= feedback.revision))
      return;
    const bool reset = value.epoch != feedback.epoch ||
                       value.media_generation != feedback.media_generation ||
                       enabled(value) != enabled(feedback);
    if (reset) {
      boundary = SteadyClock::now();
      ++generation;
      if (stats.active) {
        stop_readers();
        if (enabled(value))
          start_readers();
      }
    }
    if (encoded) {
      const auto current = value.encodings.find(descriptor.encodings.front().id);
      const auto old = feedback.encodings.find(descriptor.encodings.front().id);
      if (current != value.encodings.end())
        encoded->feedback(
            current->second.keyframe_required &&
                (old == feedback.encodings.end() ||
                 current->second.keyframe_requests != old->second.keyframe_requests || reset),
            current->second.target_bitrate_bps);
    }
    if (!enabled(value))
      capture_stop = true;
    feedback = value;
    changed.notify_all();
  }

  Token token() const {
    std::lock_guard lock(mutex);
    return {feedback.epoch, generation};
  }

  Token source_token(uint64_t source_generation) const {
    std::lock_guard lock(mutex);
    const auto current = options.shared_frames  ? options.shared_frames->generation()
                         : options.video_source ? options.video_source->stats().generation
                         : options.audio_source ? options.audio_source->stats().generation
                                                : options.encoded_source->stats().generation;
    return source_generation == current ? Token{feedback.epoch, generation} : Token{0, 0};
  }

  bool admit(Token token, SteadyClock::time_point captured) {
    std::lock_guard lock(mutex);
    if (!stats.active || stop || !enabled(feedback) || !token.epoch ||
        token.epoch != feedback.epoch || token.generation != generation || captured < boundary) {
      ++stats.dropped;
      return false;
    }
    ++stats.emitted;
    return true;
  }

  uint64_t ticks(SteadyClock::time_point captured) const {
    const auto us = std::max<int64_t>(
        0, std::chrono::duration_cast<std::chrono::microseconds>(captured - origin).count());
    return uint64_t(us / 1000000) * descriptor.format.clock_rate +
           uint64_t(us % 1000000) * descriptor.format.clock_rate / 1000000;
  }

  bool pace(std::stop_token cancel, SteadyClock::time_point when) {
    std::unique_lock lock(mutex);
    changed.wait_until(lock, cancel, when, [&] { return stop.load(); });
    return !stop && !cancel.stop_requested();
  }

  bool wait_enabled(std::stop_token cancel) {
    std::unique_lock lock(mutex);
    changed.wait(lock, cancel, [&] { return stop || enabled(feedback); });
    return !stop && !cancel.stop_requested();
  }

  void pixels(std::shared_ptr<const VideoFrame> frame, SteadyClock::time_point captured,
              Token binding) {
    if (!admit(binding, captured))
      return;
    VideoFrameEvent event;
    event.pixels = std::move(frame);
    event.captured = captured;
    event.epoch = binding.epoch;
    event.timestamp = ticks(captured);
    owner.publish(event);
  }

  void device_frame(const webrtc::VideoFrame& frame) {
    const auto binding = token();
    const auto captured = SteadyClock::now() - std::chrono::microseconds(std::max<int64_t>(
                                                   0, webrtc::TimeMicros() - frame.timestamp_us()));
    auto buffer = frame.video_frame_buffer()->ToI420();
    if (!buffer)
      throw std::runtime_error("Capture did not produce convertible video");
    if (frame.rotation() != webrtc::kVideoRotation_0)
      buffer = webrtc::I420Buffer::Rotate(*buffer, frame.rotation());
    if (buffer->width() != descriptor.format.width ||
        buffer->height() != descriptor.format.height) {
      auto scaled = webrtc::I420Buffer::Create(descriptor.format.width, descriptor.format.height);
      scaled->ScaleFrom(*buffer);
      buffer = scaled;
    }
    auto out = std::make_shared<VideoFrame>();
    out->width = buffer->width();
    out->height = buffer->height();
    out->stride_y = out->width;
    out->stride_u = out->stride_v = (out->width + 1) / 2;
    const auto copy = [](auto& dst, const uint8_t* src, int stride, int width, int height) {
      dst.resize(size_t(width) * height);
      for (int y = 0; y < height; ++y)
        std::memcpy(dst.data() + size_t(y) * width, src + size_t(y) * stride, width);
    };
    copy(out->y, buffer->DataY(), buffer->StrideY(), out->width, out->height);
    copy(out->u, buffer->DataU(), buffer->StrideU(), out->stride_u, (out->height + 1) / 2);
    copy(out->v, buffer->DataV(), buffer->StrideV(), out->stride_v, (out->height + 1) / 2);
    pixels(std::move(out), captured, binding);
  }

  void emit_audio(AudioFrameEvent& event, Token binding) {
    event.discontinuity = !audio_generation || *audio_generation != binding.generation;
    audio_generation = binding.generation;
    if (audio_processor) {
      audio_processor->process(event);
    }
    owner.publish(event);
  }

  void shared(std::stop_token cancel) {
    while (!stop && !cancel.stop_requested()) {
      if (!wait_enabled(cancel)) {
        break;
      }
      auto sample = options.shared_frames->read(cancel);
      if (!sample) {
        continue;
      }
      const auto binding = source_token(sample->generation);
      if (auto pixels_value = std::get_if<std::shared_ptr<const VideoFrame>>(&sample->payload)) {
        pixels(*pixels_value, sample->captured, binding);
      } else if (admit(binding, sample->captured)) {
        AudioFrameEvent event;
        event.pcm = std::get<std::shared_ptr<const PcmAudioFrame>>(std::move(sample->payload));
        event.captured = sample->captured;
        event.epoch = binding.epoch;
        event.timestamp = sample->timestamp;
        emit_audio(event, binding);
      }
    }
  }

  void application(std::stop_token cancel) {
    auto next = SteadyClock::now();
    std::optional<SteadyClock::time_point> audio_origin;
    uint64_t audio_base = 0, audio_generation = 0, rtp_unwrapped = 0, rtp_generation = 0;
    std::optional<uint32_t> rtp_last;
    while (!stop && !cancel.stop_requested()) {
      if (!wait_enabled(cancel))
        break;
      if (next < SteadyClock::now() - 100ms)
        next = SteadyClock::now();
      if (!encoded && !pace(cancel, next))
        break;
      if (audio && SteadyClock::now() > next + 20ms) {
        const auto skipped = (SteadyClock::now() - next) / 10ms;
        audio->skip(size_t(skipped));
        next += 10ms * skipped;
      }
      if (video) {
        auto input = video->read(cancel);
        if (!input) {
          if (options.video_source->stats().closed)
            break;
          else
            continue;
        }
        if (!pace(cancel, input->captured))
          break;
        pixels(std::shared_ptr<const VideoFrame>(input, &input->frame), input->captured,
               source_token(input->frame.stream_generation));
        next = SteadyClock::now() + std::chrono::microseconds(1000000 / options.fps);
      } else if (audio) {
        auto input = audio->read(cancel);
        if (!input) {
          if (options.audio_source->stats().closed)
            break;
          else
            continue;
        }
        const auto binding = source_token(input->generation);
        if (!audio_origin || input->generation != audio_generation) {
          audio_origin = input->captured;
          audio_base = ticks(input->captured);
          audio_generation = input->generation;
          next = SteadyClock::now();
        }
        if (admit(binding, input->captured)) {
          auto pcm = std::make_shared<PcmAudioFrame>();
          pcm->sample_rate = options.audio_source->options().sample_rate;
          pcm->channels = descriptor.format.channels;
          pcm->samples = std::move(input->samples);
          AudioFrameEvent event;
          event.pcm = std::move(pcm);
          event.epoch = binding.epoch;
          event.captured = input->captured;
          event.timestamp = audio_base + uint64_t((input->captured - *audio_origin) / 10ms) * 480;
          emit_audio(event, binding);
        }
        next += 10ms;
      } else {
        auto input = encoded->read(cancel);
        if (!input) {
          if (options.encoded_source->stats().closed)
            break;
          else
            continue;
        }
        const auto binding = source_token(input->generation);
        if (!rtp_last || input->generation != rtp_generation) {
          rtp_last.reset();
          rtp_unwrapped = input->rtp_timestamp;
          rtp_generation = input->generation;
        } else {
          const uint32_t delta = input->rtp_timestamp - *rtp_last;
          if (!delta || delta > 0x7fffffffu) {
            encoded->feedback(descriptor.kind == MediaKind::video);
            std::lock_guard lock(mutex);
            ++stats.dropped;
            continue;
          }
          rtp_unwrapped += delta;
        }
        if (!admit(binding, input->captured))
          continue;
        rtp_last = input->rtp_timestamp;
        EncodedFrame frame;
        frame.buffer = EncodedBuffer(std::move(input->data));
        frame.encoding_id = descriptor.encodings.front().id;
        frame.timestamp = rtp_unwrapped;
        frame.duration = input->duration;
        frame.captured = input->captured;
        frame.epoch = binding.epoch;
        frame.keyframe = input->keyframe;
        owner.publish(EncodedFrameEvent(std::move(frame)));
      }
    }
  }

  void generated_or_file(std::stop_token cancel) {
    const bool is_video = descriptor.kind == MediaKind::video;
    std::shared_ptr<lk::Process> child;
    if (!options.generated) {
      std::vector<std::string> args{
          options.ffmpeg,    "-nostdin", "-hide_banner", "-loglevel", "error", "-threads", "1",
          "-filter_threads", "1",        "-rw_timeout",  "10000000"};
      if (options.loop)
        args.insert(args.end(), {"-stream_loop", "-1"});
      auto in = input_args(options.input);
      args.insert(args.end(), in.begin(), in.end());
      if (is_video)
        args.insert(args.end(), {"-map", "0:v:0", "-an", "-vf",
                                 "scale=" + std::to_string(descriptor.format.width) + ":" +
                                     std::to_string(descriptor.format.height) +
                                     ":force_original_aspect_ratio=decrease,pad=" +
                                     std::to_string(descriptor.format.width) + ":" +
                                     std::to_string(descriptor.format.height) +
                                     ":(ow-iw)/2:(oh-ih)/2,fps=" + std::to_string(options.fps),
                                 "-pix_fmt", "yuv420p", "-f", "rawvideo", "pipe:1"});
      else
        args.insert(args.end(),
                    {"-map", "0:a:0", "-vn", "-ac", std::to_string(descriptor.format.channels),
                     "-ar", "48000", "-f", "s16le", "pipe:1"});
      std::lock_guard lock(mutex);
      if (stop)
        return;
      process = child = std::make_shared<lk::Process>(args);
    }
    const auto period = std::chrono::microseconds(is_video ? 1000000 / options.fps : 10000);
    auto next = SteadyClock::now(), first = next;
    uint64_t sequence = 0;
    const auto audio_base = ticks(first);
    while (!stop && pace(cancel, next)) {
      if (!wait_enabled(cancel))
        break;
      if (next < SteadyClock::now() - period * 5)
        next = SteadyClock::now();
      const auto binding = token();
      const auto captured = next;
      if (is_video) {
        auto out = std::make_shared<VideoFrame>();
        out->width = descriptor.format.width;
        out->height = descriptor.format.height;
        out->stride_y = out->width;
        out->stride_u = out->stride_v = (out->width + 1) / 2;
        out->y.resize(size_t(out->width) * out->height);
        out->u.resize(size_t(out->stride_u) * ((out->height + 1) / 2));
        out->v.resize(out->u.size());
        if (child) {
          if (!child->read_exact(out->y.data(), out->y.size()))
            break;
          if (!child->read_exact(out->u.data(), out->u.size()) ||
              !child->read_exact(out->v.data(), out->v.size()))
            throw std::runtime_error("Truncated I420 frame");
        } else {
          for (int y = 0; y < out->height; ++y)
            for (int x = 0; x < out->width; ++x)
              out->y[size_t(y) * out->width + x] = uint8_t(16 + (x + y + sequence * 3) % 220);
          std::fill(out->u.begin(), out->u.end(), 96);
          std::fill(out->v.begin(), out->v.end(), 160);
        }
        pixels(std::move(out), captured, binding);
      } else {
        auto pcm = std::make_shared<PcmAudioFrame>();
        pcm->channels = descriptor.format.channels;
        pcm->samples.resize(480 * pcm->channels);
        if (child) {
          size_t used = 0, bytes = pcm->samples.size() * sizeof(int16_t);
          while (used < bytes) {
            const auto n =
                child->read(reinterpret_cast<char*>(pcm->samples.data()) + used, bytes - used);
            if (!n)
              break;
            used += n;
          }
          if (!used)
            break;
          if (used % (sizeof(int16_t) * pcm->channels))
            throw std::runtime_error("Truncated PCM sample frame");
        } else
          for (int i = 0; i < 480; ++i)
            for (uint16_t ch = 0; ch < pcm->channels; ++ch)
              pcm->samples[i * pcm->channels + ch] =
                  int16_t(6000 * std::sin(2 * std::numbers::pi * (440 + ch * 220) *
                                          (sequence * 480 + i) / 48000.0));
        if (admit(binding, captured)) {
          AudioFrameEvent event;
          event.pcm = std::move(pcm);
          event.captured = captured;
          event.epoch = binding.epoch;
          event.timestamp =
              audio_base +
              uint64_t(
                  std::chrono::duration_cast<std::chrono::milliseconds>(captured - first).count() /
                  10) *
                  480;
          emit_audio(event, binding);
        }
      }
      ++sequence;
      next += period;
      if (next < SteadyClock::now() - period * 5)
        next = SteadyClock::now();
    }
    if (child && !stop && child->wait() != 0)
      throw std::runtime_error("FFmpeg input failed");
    if (!stop && !sequence)
      throw std::runtime_error("Input produced no media frames");
  }

  void run(std::stop_token cancel) {
    try {
      // No media is read or generated before the first binding feedback.
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, cancel, [&] { return stop || feedback.epoch; });
      }
      if (!stop && !cancel.stop_requested()) {
        if (options.microphone)
          options.microphone->start();
        if (options.shared_frames) {
          shared(cancel);
        } else if (video || audio || encoded) {
          application(cancel);
        } else if (options.capture != CaptureKind::input) {
          MediaSpec spec;
          spec.capture = options.capture;
          spec.device_index = options.device_index;
          spec.screen_id = options.screen_id;
          spec.width = descriptor.format.width;
          spec.height = descriptor.format.height;
          spec.fps = options.fps;
          while (wait_enabled(cancel)) {
            {
              std::lock_guard lock(mutex);
              if (stop || !enabled(feedback))
                continue;
              capture_stop = false;
            }
            lk::capture_device(spec, capture_stop,
                               [this](const auto& frame) { device_frame(frame); });
            if (!capture_stop)
              throw std::runtime_error("Capture device stopped unexpectedly");
          }
        } else {
          generated_or_file(cancel);
        }
      }
    } catch (...) {
      if (!stop) {
        const auto error = normalized_failure(std::current_exception(),
                                              {"capture", descriptor.id, owner.getName(), 0},
                                              ErrorDomain::media, ErrorCode::processing_failed);
        {
          std::lock_guard lock(mutex);
          stats.error = describe_failure(error)->message;
        }
        owner.reportFailure(error);
      }
    }
    if (options.microphone) {
      options.microphone->stop();
    }
    std::exception_ptr device_error;
    {
      std::lock_guard lock(mutex);
      if (options.microphone && stats.error.empty() && !stop) {
        const auto device = options.microphone->snapshot();
        stats.error = device.error;
        if (device.failure) {
          device_error = std::make_exception_ptr(SDKError(device.failure));
        }
      }
      stats.finished = true;
      stats.active = false;
      stop_readers();
    }
    if (device_error) {
      owner.reportFailure(device_error);
    }
  }
};

MediaInputNode::MediaInputNode(std::string name, TrackDescriptor d, MediaInputOptions options)
    : GraphNode(name, nullptr),
      impl_(std::make_unique<Impl>(*this, std::move(d), std::move(options))) {}

MediaInputNode::~MediaInputNode() {
  quiesce();
  release();
}

bool MediaInputNode::doInit() {
  subscribe<SenderFeedbackEvent>([this](const auto& value) { impl_->update(value.feedback); });
  return true;
}

bool MediaInputNode::doStart() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed)
    return false;
  impl_->started = true;
  return true;
}

void MediaInputNode::activate() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed || !impl_->started)
    throw std::logic_error("Start media input graph before activation");
  if (impl_->worker.joinable())
    return;
  impl_->start_readers();
  if (impl_->options.microphone) {
    impl_->options.microphone->set_failure_handler(failureReporter());
    impl_->microphone_reporter = true;
  }
  impl_->stats.active = true;
  impl_->worker = std::jthread([this](std::stop_token cancel) { impl_->run(cancel); });
}

void MediaInputNode::quiesce() {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->closed = true;
    impl_->stop = true;
    impl_->capture_stop = true;
    impl_->stats.active = false;
    impl_->stop_readers();
    if (impl_->process)
      impl_->process->terminate();
  }
  impl_->worker.request_stop();
  impl_->changed.notify_all();
  if (impl_->worker.joinable())
    impl_->worker.join();
  std::lock_guard lock(impl_->mutex);
  if (impl_->microphone_reporter) {
    impl_->options.microphone->set_failure_handler({});
    impl_->microphone_reporter = false;
  }
  impl_->video.reset();
  impl_->audio.reset();
  impl_->encoded.reset();
  impl_->process.reset();
}

void MediaInputNode::doStop() {
  quiesce();
}

MediaInputSnapshot MediaInputNode::snapshot() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}

void MediaInputNode::throw_if_failed() const {
  if (auto error = failure()) {
    std::rethrow_exception(error);
  }
}
}  // namespace livekit::sdk::rtc::nodes
