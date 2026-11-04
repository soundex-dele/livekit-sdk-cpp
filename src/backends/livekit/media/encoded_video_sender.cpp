#include "backends/livekit/media/encoded_video_sender.hpp"
#include "backends/livekit/media/queue_budget.hpp"
#include "backends/livekit/media/codec_envelopes.hpp"
#include "backends/livekit/media/video_receive_bridge.hpp"
#include "backends/livekit/track_state.hpp"
#include "livekit/media.hpp"
#include "api/video/i420_buffer.h"
#include "api/make_ref_counted.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "system_wrappers/include/clock.h"
#include "system_wrappers/include/ntp_time.h"
#include <algorithm>
#include <atomic>
#include <mutex>

namespace livekit::sdk::client::detail {
namespace {
class BudgetedImage : public webrtc::EncodedImageBufferInterface {
 public:
  explicit BudgetedImage(const EncodedBuffer& buffer)
      : bytes_(buffer.bytes().begin(), buffer.bytes().end()),
        reservation_(QueueBudget::ownership(buffer)) {}

  const uint8_t* data() const override {
    return bytes_.data();
  }

  uint8_t* data() override {
    return bytes_.data();
  }

  size_t size() const override {
    return bytes_.size();
  }

 private:
  std::vector<uint8_t> bytes_;
  std::shared_ptr<void> reservation_;
};
}  // namespace

struct VideoSenderState {
  Epoch epoch;
  TrackConfig descriptor;
  mutable std::mutex mutex;
  SenderFeedback feedback;
  std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup;
  size_t outstanding_frames = 0, outstanding_bytes = 0;
  bool closed = false;
  bool muted = false;
  bool demanded = true;
  uint64_t generation = 0;
  std::optional<uint64_t> last_timestamp;
  int64_t last_capture_us = 0;

  void request_keyframe() {
    std::lock_guard lock(mutex);
    if (closed) {
      return;
    }
    auto& encoding = feedback.encodings.begin()->second;
    encoding.keyframe_required = true;
    ++encoding.keyframe_requests;
    ++feedback.revision;
    if (wakeup) {
      wakeup->signal();
    }
  }
};

namespace {
using namespace webrtc;

VideoCodecType native_codec(Codec codec) {
  switch (codec) {
    case Codec::vp8:
      return kVideoCodecVP8;
    case Codec::h264:
      return kVideoCodecH264;
    case Codec::h265:
      return kVideoCodecH265;
    default:
      return kVideoCodecGeneric;
  }
}

std::vector<SdpVideoFormat> video_formats() {
  return {SdpVideoFormat("VP8"),
          SdpVideoFormat("H264", {{"profile-level-id", "42e01f"},
                                  {"packetization-mode", "1"},
                                  {"level-asymmetry-allowed", "1"}}),
          SdpVideoFormat("H265", {{"profile-id", "1"}, {"tier-flag", "0"}, {"level-id", "93"}})};
}

class PacketBuffer : public VideoFrameBuffer {
 public:
  PacketBuffer(std::shared_ptr<VideoSenderState> state, EncodedFrame frame)
      : state(std::move(state)), frame(std::move(frame)) {}

  ~PacketBuffer() override {
    if (!reserved) {
      return;
    }
    if (!consumed) {
      state->request_keyframe();
    }
    std::lock_guard lock(state->mutex);
    --state->outstanding_frames;
    state->outstanding_bytes -= frame.buffer.size();
    if (state->wakeup) {
      state->wakeup->signal();
    }
  }

  Type type() const override {
    return Type::kNative;
  }

  int width() const override {
    return int(state->descriptor.format.width);
  }

  int height() const override {
    return int(state->descriptor.format.height);
  }

  scoped_refptr<I420BufferInterface> ToI420() override {
    return nullptr;
  }

  std::shared_ptr<VideoSenderState> state;
  EncodedFrame frame;
  bool reserved = false;
  uint64_t generation = 0;
  std::atomic<bool> consumed{false};
  const SteadyClock::time_point enqueued = QueueBudget::admitted(frame.buffer);
};

class PacketEncoder final : public VideoEncoder {
 public:
  int InitEncode(const VideoCodec* codec, const Settings&) override {
    if (!codec || codec->numberOfSimulcastStreams > 1 ||
        (codec->codecType != kVideoCodecVP8 && codec->codecType != kVideoCodecH264 &&
         codec->codecType != kVideoCodecH265)) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    codec_ = *codec;
    active_ = true;
    OnKeyFrameRequested();
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t RegisterEncodeCompleteCallback(EncodedImageCallback* callback) override {
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Encode(const webrtc::VideoFrame& frame,
                 const std::vector<VideoFrameType>* types) override {
    auto* packet = dynamic_cast<PacketBuffer*>(frame.video_frame_buffer().get());
    if (!packet) {
      // Invalid local input still requires recovery even when remote refresh
      // requests are ignored for an externally encoded stream.
      if (auto state = state_.lock()) {
        state->request_keyframe();
      }
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    if (packet->consumed.exchange(true)) {
      return WEBRTC_VIDEO_CODEC_OK;
    }
    if (state_.lock() != packet->state) {
      state_ = packet->state;
      update_rates();
    }
    if (SteadyClock::now() - packet->enqueued > packet->state->descriptor.queue.max_age) {
      packet->state->request_keyframe();
      if (callback_) {
        callback_->OnDroppedFrame(EncodedImageCallback::DropReason::kDroppedByEncoder);
      }
      return WEBRTC_VIDEO_CODEC_OK;
    }
#if defined(LIVEKIT_IGNORE_REMOTE_VIDEO_KEYFRAME_REQUESTS)
    // This producer supplies periodic IDRs; a remote refresh request must not
    // turn uninterrupted encoded P frames into a local recovery gap.
    (void)types;
    const bool requested = false;
#else
    const bool requested = types && std::find(types->begin(), types->end(),
                                              VideoFrameType::kVideoFrameKey) != types->end();
#endif
    bool needs_keyframe;
    uint64_t request_revision;
    {
      std::lock_guard lock(packet->state->mutex);
      if (packet->state->closed || packet->state->muted || !packet->state->demanded ||
          packet->generation != packet->state->generation ||
          packet->frame.epoch != packet->state->epoch) {
        return WEBRTC_VIDEO_CODEC_OK;
      }
      needs_keyframe = packet->state->feedback.encodings.begin()->second.keyframe_required;
      request_revision = packet->state->feedback.encodings.begin()->second.keyframe_requests;
    }
    if (!active_ || !callback_ ||
        native_codec(packet->state->descriptor.format.codec) != codec_.codecType ||
        packet->width() != codec_.width || packet->height() != codec_.height) {
      packet->state->request_keyframe();
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    if ((requested || needs_keyframe) && !packet->frame.keyframe) {
      packet->state->request_keyframe();
      callback_->OnDroppedFrame(EncodedImageCallback::DropReason::kDroppedByEncoder);
      return WEBRTC_VIDEO_CODEC_OK;
    }
    EncodedImage image;
    image.SetEncodedData(webrtc::make_ref_counted<BudgetedImage>(packet->frame.buffer));
    image._encodedWidth = packet->width();
    image._encodedHeight = packet->height();
    image._frameType =
        packet->frame.keyframe ? VideoFrameType::kVideoFrameKey : VideoFrameType::kVideoFrameDelta;
    image.SetRtpTimestamp(frame.rtp_timestamp());
    CodecSpecificInfo info{};
    info.codecType = codec_.codecType;
    if (info.codecType == kVideoCodecVP8) {
      info.codecSpecific.VP8.temporalIdx = kNoTemporalIdx;
      info.codecSpecific.VP8.keyIdx = kNoKeyIdx;
    }
    if (info.codecType == kVideoCodecH264) {
      info.codecSpecific.H264.packetization_mode = H264PacketizationMode::NonInterleaved;
    }
    {
      std::lock_guard lock(packet->state->mutex);
      if (packet->state->closed || packet->state->muted || !packet->state->demanded ||
          packet->generation != packet->state->generation) {
        return WEBRTC_VIDEO_CODEC_OK;
      }
    }
    const auto result = callback_->OnEncodedImage(image, &info);
    if (result.error != EncodedImageCallback::Result::OK || result.drop_next_frame) {
      packet->state->request_keyframe();
    } else if (packet->frame.keyframe) {
      std::lock_guard lock(packet->state->mutex);
      auto& encoding = packet->state->feedback.encodings.begin()->second;
      if (encoding.keyframe_requests == request_revision) {
        encoding.keyframe_required = false;
        ++packet->state->feedback.revision;
        if (packet->state->wakeup) {
          packet->state->wakeup->signal();
        }
      }
    }
    return result.error == EncodedImageCallback::Result::OK ? WEBRTC_VIDEO_CODEC_OK
                                                            : WEBRTC_VIDEO_CODEC_ERROR;
  }

  void OnKeyFrameRequested() override {
#if !defined(LIVEKIT_IGNORE_REMOTE_VIDEO_KEYFRAME_REQUESTS)
    if (auto state = state_.lock()) {
      state->request_keyframe();
    }
#endif
  }

  void OnLossNotification(const LossNotification&) override {
    OnKeyFrameRequested();
  }

  void SetRates(const RateControlParameters& rates) override {
    bitrate_ = rates.bitrate.get_sum_bps();
    update_rates();
  }

  EncoderInfo GetEncoderInfo() const override {
    EncoderInfo info;
    info.implementation_name = "rtc/encoded-envelope";
    info.supports_native_handle = true;
    info.has_trusted_rate_controller = true;
    info.scaling_settings = ScalingSettings::kOff;
    info.is_qp_trusted = true;
    return info;
  }

  int32_t Release() override {
    OnKeyFrameRequested();
    callback_ = nullptr;
    active_ = false;
    state_.reset();
    return WEBRTC_VIDEO_CODEC_OK;
  }

 private:
  void update_rates() {
    if (!bitrate_) {
      return;
    }
    if (auto state = state_.lock()) {
      std::lock_guard lock(state->mutex);
      auto& encoding = state->feedback.encodings.begin()->second;
      if (encoding.target_bitrate_bps != *bitrate_ || encoding.active != (*bitrate_ > 0)) {
        encoding.target_bitrate_bps = *bitrate_;
        encoding.active = *bitrate_ > 0;
        ++state->feedback.revision;
        if (state->wakeup) {
          state->wakeup->signal();
        }
      }
    }
  }

  VideoCodec codec_{};
  EncodedImageCallback* callback_ = nullptr;
  std::weak_ptr<VideoSenderState> state_;
  std::optional<uint32_t> bitrate_;
  bool active_ = false;
};

class EncoderFactory final : public VideoEncoderFactory {
 public:
  std::vector<SdpVideoFormat> GetSupportedFormats() const override {
    return video_formats();
  }

  CodecSupport QueryCodecSupport(const SdpVideoFormat& format,
                                 std::optional<std::string> mode) const override {
    return {(!mode || *mode == "L1T1") && format.IsCodecInList(video_formats()), false};
  }

  std::unique_ptr<VideoEncoder> Create(const Environment&, const SdpVideoFormat& format) override {
    return format.IsCodecInList(video_formats()) ? std::make_unique<PacketEncoder>() : nullptr;
  }
};

struct DecodeCallbackState {
  std::mutex mutex;
  DecodedImageCallback* callback = nullptr;
};

class DecodeCompletion final : public media::VideoDecodeCompletion {
 public:
  DecodeCompletion(std::weak_ptr<DecodeCallbackState> state, std::weak_ptr<VideoReceiveRoute> route,
                   uint64_t generation, std::weak_ptr<TrackData> track, Epoch epoch,
                   uint32_t timestamp)
      : state_(std::move(state)),
        route_(std::move(route)),
        generation_(generation),
        track_(std::move(track)),
        epoch_(epoch),
        timestamp_(timestamp) {}

  void decoded(std::shared_ptr<const livekit::sdk::VideoFrame> pixels) override {
    if (!pixels || completed_.exchange(true)) {
      return;
    }
    auto route = route_.lock();
    auto track = track_.lock();
    auto state = state_.lock();
    if (!route || !track || !state) {
      return;
    }
    {
      std::lock_guard lock(route->mutex);
      if (route->generation != generation_ || route->track.lock() != track) {
        return;
      }
    }
    {
      std::lock_guard lock(track->mutex);
      if (track->info.epoch != epoch_ || track->info.state != TrackState::ready) {
        return;
      }
    }
    const auto& p = *pixels;
    if (p.width <= 0 || p.height <= 0 || p.width > 8192 || p.height > 8192) {
      return;
    }
    const auto valid = [](const auto& plane, int stride, int width, int height) {
      return stride >= width && size_t(stride) * (height - 1) + width <= plane.size();
    };
    const int cw = (p.width + 1) / 2, ch = (p.height + 1) / 2;
    if (!valid(p.y, p.stride_y, p.width, p.height) || !valid(p.u, p.stride_u, cw, ch) ||
        !valid(p.v, p.stride_v, cw, ch)) {
      return;
    }
    auto buffer = I420Buffer::Copy(p.width, p.height, p.y.data(), p.stride_y, p.u.data(),
                                   p.stride_u, p.v.data(), p.stride_v);
    auto frame = webrtc::VideoFrame::Builder()
                     .set_video_frame_buffer(buffer)
                     .set_rtp_timestamp(timestamp_)
                     .set_timestamp_us(p.timestamp_us)
                     .build();
    // Release synchronizes with in-flight completions before WebRTC destroys
    // its callback. No room/graph/user callbacks run under this lock.
    std::lock_guard lock(state->mutex);
    if (state->callback) {
      state->callback->Decoded(frame);
    }
  }

 private:
  std::weak_ptr<DecodeCallbackState> state_;
  std::weak_ptr<VideoReceiveRoute> route_;
  uint64_t generation_;
  std::weak_ptr<TrackData> track_;
  Epoch epoch_;
  uint32_t timestamp_;
  std::atomic<bool> completed_{false};
};

class DecoderBoundary final : public VideoDecoder {
 public:
  explicit DecoderBoundary(std::shared_ptr<VideoReceiveRoutes> routes)
      : routes_(std::move(routes)), state_(std::make_shared<DecodeCallbackState>()) {}

  ~DecoderBoundary() override {
    Release();
  }

  bool Configure(const Settings&) override {
    return true;
  }

  int32_t Decode(const EncodedImage& image, int64_t) override {
    if (image.PacketInfos().empty() || !image.size()) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    auto route = routes_->find(image.PacketInfos().begin()->ssrc());
    if (!route) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }
    std::lock_guard lock(route->mutex);
    auto track = route->track.lock();
    if (!track || !track->budget) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }
    auto payload = track->budget->copy({image.data(), image.size()});
    if (!payload) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    const auto current = image.RtpTimestamp();
    uint32_t ticks = 0;
    if (route->previous) {
      const int32_t delta = int32_t(current - *route->previous);
      if (delta < 0) {
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
      ticks = uint32_t(delta);
      route->timestamp += ticks;
    } else {
      route->timestamp = current;
    }
    route->previous = current;
    EncodedFrame frame;
    frame.buffer = std::move(*payload);
    frame.timestamp = route->timestamp;
    frame.duration = std::chrono::microseconds(uint64_t(ticks) * 1000000 / 90000);
    frame.keyframe = image.FrameType() == VideoFrameType::kVideoFrameKey;
    for (const auto& packet : image.PacketInfos()) {
      if (auto capture = packet.absolute_capture_time();
          capture && capture->absolute_capture_timestamp) {
        frame.sender_capture_time_us = UQ32x32ToInt64Us(capture->absolute_capture_timestamp);
        break;
      }
    }
    const auto snapshot = track->snapshot();
    frame.decode_completion = std::make_shared<DecodeCompletion>(state_, route, route->generation,
                                                                 track, snapshot.epoch, current);
    // OK means accepted for asynchronous external decoding, not fabricated
    // decode success. Only the real VideoCodecNode output invokes Decoded().
    return track->receive(std::move(frame)) ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_ERROR;
  }

  int32_t RegisterDecodeCompleteCallback(DecodedImageCallback* callback) override {
    auto next = std::make_shared<DecodeCallbackState>();
    next->callback = callback;
    auto old = std::exchange(state_, std::move(next));
    std::lock_guard lock(old->mutex);
    old->callback = nullptr;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() override {
    auto old = std::exchange(state_, std::make_shared<DecodeCallbackState>());
    std::lock_guard lock(old->mutex);
    old->callback = nullptr;
    return WEBRTC_VIDEO_CODEC_OK;
  }

 private:
  std::shared_ptr<VideoReceiveRoutes> routes_;
  std::shared_ptr<DecodeCallbackState> state_;
};

class DecoderFactory final : public VideoDecoderFactory {
 public:
  explicit DecoderFactory(std::shared_ptr<VideoReceiveRoutes> routes)
      : routes_(std::move(routes)) {}

  std::vector<SdpVideoFormat> GetSupportedFormats() const override {
    return video_formats();
  }

  std::unique_ptr<VideoDecoder> Create(const Environment&, const SdpVideoFormat& format) override {
    return format.IsCodecInList(video_formats()) ? std::make_unique<DecoderBoundary>(routes_)
                                                 : nullptr;
  }

 private:
  std::shared_ptr<VideoReceiveRoutes> routes_;
};
}  // namespace

EncodedVideoSender::EncodedVideoSender(Epoch epoch, TrackConfig descriptor,
                                       std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup)
    : VideoTrackSource(false), state_(std::make_shared<VideoSenderState>()) {
  if (descriptor.kind != MediaKind::video || descriptor.encodings.size() != 1 ||
      native_codec(descriptor.format.codec) == webrtc::kVideoCodecGeneric ||
      !descriptor.format.width || !descriptor.format.height || descriptor.format.width > 16384 ||
      descriptor.format.height > 16384 || descriptor.format.clock_rate != 90000 ||
      !descriptor.queue.max_frames || !descriptor.queue.max_bytes ||
      descriptor.queue.max_age.count() <= 0) {
    failure_ =
        make_failure(ErrorDomain::transport, ErrorCode::invalid_argument,
                     "Encoded video transport requires one encoding and a valid video descriptor");
    state_->closed = true;
    return;
  }
  state_->wakeup = wakeup;
  broadcaster_.wakeup = std::move(wakeup);
  state_->epoch = epoch;
  state_->descriptor = std::move(descriptor);
  state_->muted = state_->descriptor.muted;
  state_->feedback.epoch = state_->epoch;
  state_->feedback.revision = 1;
  auto& encoding = state_->feedback.encodings[state_->descriptor.encodings.front().id];
  encoding.keyframe_required = true;
  encoding.keyframe_requests = 1;
  encoding.target_bitrate_bps = state_->descriptor.encodings.front().max_bitrate_bps;
}

EncodedVideoSender::~EncodedVideoSender() {
  close();
}

WriteStatus EncodedVideoSender::send(const EncodedFrame& frame) {
  {
    std::lock_guard lock(state_->mutex);
    if (state_->closed || state_->muted || !state_->demanded) {
      return WriteStatus::not_ready;
    }
  }
  if (!broadcaster_.frame_wanted()) {
    return WriteStatus::backpressure;
  }
  auto packet = webrtc::make_ref_counted<PacketBuffer>(state_, frame);
  int64_t capture_us;
  {
    std::lock_guard lock(state_->mutex);
    const auto& descriptor = state_->descriptor;
    if (state_->closed || state_->muted || !state_->demanded || frame.epoch != state_->epoch ||
        frame.config_revision != 1 || frame.encoding_id != descriptor.encodings.front().id ||
        frame.buffer.size() == 0 || frame.buffer.size() > descriptor.queue.max_bytes ||
        (state_->last_timestamp && frame.timestamp <= *state_->last_timestamp)) {
      return WriteStatus::not_ready;
    }
    if (state_->outstanding_frames >= descriptor.queue.max_frames ||
        frame.buffer.size() > descriptor.queue.max_bytes - state_->outstanding_bytes) {
      return WriteStatus::backpressure;
    }
    // The native capture clock orders triggers; the explicit encoded RTP
    // timestamp preserves the external media clock without millisecond loss.
    const auto age_us = frame.captured ? std::chrono::duration_cast<std::chrono::microseconds>(
                                             SteadyClock::now() - *frame.captured)
                                             .count()
                                       : 0;
    capture_us = std::max(webrtc::Clock::GetRealTimeClock()->TimeInMicroseconds() - age_us,
                          state_->last_capture_us + 1000);
    state_->last_capture_us = capture_us;
    state_->last_timestamp = frame.timestamp;
    ++state_->outstanding_frames;
    state_->outstanding_bytes += frame.buffer.size();
    packet->reserved = true;
    packet->generation = state_->generation;
  }
  auto native = webrtc::VideoFrame::Builder()
                    .set_video_frame_buffer(packet)
                    .set_timestamp_us(capture_us)
                    .set_rotation(webrtc::kVideoRotation_0)
                    .build();
  native.set_encoded_rtp_timestamp(uint32_t(frame.timestamp));
  broadcaster_.OnFrame(native);
  return WriteStatus::accepted;
}

SenderFeedback EncodedVideoSender::feedback() const {
  std::lock_guard lock(state_->mutex);
  auto feedback = state_->feedback;
  if (state_->muted || !state_->demanded) {
    for (auto& [id, encoding] : feedback.encodings) {
      encoding.active = false;
    }
  }
  return feedback;
}

void EncodedVideoSender::set_muted(bool muted) {
  std::lock_guard lock(state_->mutex);
  if (state_->closed || state_->muted == muted) {
    return;
  }
  state_->muted = muted;
  ++state_->feedback.media_generation;
  ++state_->generation;
  for (auto& [id, encoding] : state_->feedback.encodings) {
    encoding.keyframe_required = true;
    ++encoding.keyframe_requests;
  }
  ++state_->feedback.revision;
  if (state_->wakeup) {
    state_->wakeup->signal();
  }
}

void EncodedVideoSender::close() {
  std::lock_guard lock(state_->mutex);
  state_->closed = true;
}

void EncodedVideoSender::set_demand(bool enabled) {
  std::lock_guard lock(state_->mutex);
  if (state_->closed || state_->demanded == enabled) {
    return;
  }
  state_->demanded = enabled;
  ++state_->generation;
  for (auto& [id, encoding] : state_->feedback.encodings) {
    encoding.keyframe_required = true;
    ++encoding.keyframe_requests;
  }
  ++state_->feedback.revision;
  if (state_->wakeup) {
    state_->wakeup->signal();
  }
}

std::unique_ptr<webrtc::VideoEncoderFactory> create_transport_video_encoder_factory() {
  return std::make_unique<EncoderFactory>();
}

std::unique_ptr<webrtc::VideoDecoderFactory> create_transport_video_decoder_factory(
    std::shared_ptr<VideoReceiveRoutes> routes) {
  return std::make_unique<DecoderFactory>(std::move(routes));
}
}  // namespace livekit::sdk::client::detail
