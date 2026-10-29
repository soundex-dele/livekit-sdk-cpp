#include "backends/rtc/encoded_video_sender.hpp"
#include "backends/rtc/queue_budget.hpp"
#include "backends/rtc/transport_codecs.hpp"
#include "api/make_ref_counted.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "system_wrappers/include/clock.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace livekit::sdk::rtc::detail {
namespace {
class BudgetedImage : public webrtc::EncodedImageBufferInterface {
 public:
  explicit BudgetedImage(const EncodedBuffer& buffer)
      : bytes_(buffer.bytes().begin(), buffer.bytes().end()), reservation_(QueueBudget::ownership(buffer)) {}
  const uint8_t* data() const override { return bytes_.data(); }
  uint8_t* data() override { return bytes_.data(); }
  size_t size() const override { return bytes_.size(); }
 private:
  std::vector<uint8_t> bytes_;
  std::shared_ptr<void> reservation_;
};
}

struct VideoSenderState {
  BindingToken binding;
  TrackDescriptor descriptor;
  mutable std::mutex mutex;
  SenderFeedback feedback;
  size_t outstanding_frames = 0, outstanding_bytes = 0;
  bool closed = false;
  bool muted = false;
  bool demanded = true;
  uint64_t generation = 0;
  std::optional<uint64_t> last_timestamp;
  int64_t last_capture_us = 0;
  void request_keyframe() {
    std::lock_guard lock(mutex);
    if (closed) return;
    auto& encoding = feedback.encodings.begin()->second;
    encoding.keyframe_required = true;
    ++encoding.keyframe_requests;
    ++feedback.revision;
  }
};
namespace {
using namespace webrtc;
VideoCodecType native_codec(Codec codec) {
  switch (codec) {
    case Codec::vp8: return kVideoCodecVP8;
    case Codec::h264: return kVideoCodecH264;
    case Codec::h265: return kVideoCodecH265;
    default: return kVideoCodecGeneric;
  }
}
std::vector<SdpVideoFormat> video_formats() {
  return {SdpVideoFormat("VP8"),
          SdpVideoFormat("H264", {{"profile-level-id", "42e01f"}, {"packetization-mode", "1"}, {"level-asymmetry-allowed", "1"}}),
          SdpVideoFormat("H265", {{"profile-id", "1"}, {"tier-flag", "0"}, {"level-id", "93"}})};
}
class PacketBuffer : public VideoFrameBuffer {
 public:
  PacketBuffer(std::shared_ptr<VideoSenderState> state, EncodedFrame frame)
      : state(std::move(state)), frame(std::move(frame)) {}
  ~PacketBuffer() override {
    if (!reserved) return;
    if (!consumed) state->request_keyframe();
    std::lock_guard lock(state->mutex);
    --state->outstanding_frames;
    state->outstanding_bytes -= frame.buffer.size();
  }
  Type type() const override { return Type::kNative; }
  int width() const override { return int(state->descriptor.format.width); }
  int height() const override { return int(state->descriptor.format.height); }
  scoped_refptr<I420BufferInterface> ToI420() override { return nullptr; }
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
        (codec->codecType != kVideoCodecVP8 && codec->codecType != kVideoCodecH264 && codec->codecType != kVideoCodecH265))
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    codec_ = *codec;
    active_ = true;
    OnKeyFrameRequested();
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t RegisterEncodeCompleteCallback(EncodedImageCallback* callback) override {
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Encode(const VideoFrame& frame, const std::vector<VideoFrameType>* types) override {
    auto* packet = dynamic_cast<PacketBuffer*>(frame.video_frame_buffer().get());
    if (!packet) { OnKeyFrameRequested(); return WEBRTC_VIDEO_CODEC_ERR_PARAMETER; }
    if (packet->consumed.exchange(true)) return WEBRTC_VIDEO_CODEC_OK;
    if (state_.lock() != packet->state) {
      state_ = packet->state;
      update_rates();
    }
    if (SteadyClock::now() - packet->enqueued > packet->state->descriptor.queue.max_age) {
      packet->state->request_keyframe();
      if (callback_) callback_->OnDroppedFrame(EncodedImageCallback::DropReason::kDroppedByEncoder);
      return WEBRTC_VIDEO_CODEC_OK;
    }
    const bool requested = types && std::find(types->begin(), types->end(), VideoFrameType::kVideoFrameKey) != types->end();
    bool needs_keyframe;
    uint64_t request_revision;
    {
      std::lock_guard lock(packet->state->mutex);
      if (packet->state->closed || packet->state->muted || !packet->state->demanded || packet->generation != packet->state->generation ||
          packet->frame.epoch != packet->state->binding.epoch)
        return WEBRTC_VIDEO_CODEC_OK;
      needs_keyframe = packet->state->feedback.encodings.begin()->second.keyframe_required;
      request_revision = packet->state->feedback.encodings.begin()->second.keyframe_requests;
    }
    if (!active_ || !callback_ || native_codec(packet->state->descriptor.format.codec) != codec_.codecType ||
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
    image._frameType = packet->frame.keyframe ? VideoFrameType::kVideoFrameKey : VideoFrameType::kVideoFrameDelta;
    image.SetRtpTimestamp(frame.rtp_timestamp());
    CodecSpecificInfo info{};
    info.codecType = codec_.codecType;
    if (info.codecType == kVideoCodecVP8) {
      info.codecSpecific.VP8.temporalIdx = kNoTemporalIdx;
      info.codecSpecific.VP8.keyIdx = kNoKeyIdx;
    }
    if (info.codecType == kVideoCodecH264)
      info.codecSpecific.H264.packetization_mode = H264PacketizationMode::NonInterleaved;
    {
      std::lock_guard lock(packet->state->mutex);
      if (packet->state->closed || packet->state->muted || !packet->state->demanded || packet->generation != packet->state->generation)
        return WEBRTC_VIDEO_CODEC_OK;
    }
    const auto result = callback_->OnEncodedImage(image, &info);
    if (result.error != EncodedImageCallback::Result::OK || result.drop_next_frame)
      packet->state->request_keyframe();
    else if (packet->frame.keyframe) {
      std::lock_guard lock(packet->state->mutex);
      auto& encoding = packet->state->feedback.encodings.begin()->second;
      if (encoding.keyframe_requests == request_revision) {
        encoding.keyframe_required = false;
        ++packet->state->feedback.revision;
      }
    }
    return result.error == EncodedImageCallback::Result::OK ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_ERROR;
  }
  void OnKeyFrameRequested() override {
    if (auto state = state_.lock()) state->request_keyframe();
  }
  void OnLossNotification(const LossNotification&) override { OnKeyFrameRequested(); }
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
    if (!bitrate_) return;
    if (auto state = state_.lock()) {
      std::lock_guard lock(state->mutex);
      auto& encoding = state->feedback.encodings.begin()->second;
      if (encoding.target_bitrate_bps != *bitrate_ || encoding.active != (*bitrate_ > 0)) {
        encoding.target_bitrate_bps = *bitrate_;
        encoding.active = *bitrate_ > 0;
        ++state->feedback.revision;
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
  std::vector<SdpVideoFormat> GetSupportedFormats() const override { return video_formats(); }
  CodecSupport QueryCodecSupport(const SdpVideoFormat& format, std::optional<std::string> mode) const override {
    return {(!mode || *mode == "L1T1") && format.IsCodecInList(video_formats()), false};
  }
  std::unique_ptr<VideoEncoder> Create(const Environment&, const SdpVideoFormat& format) override {
    return format.IsCodecInList(video_formats()) ? std::make_unique<PacketEncoder>() : nullptr;
  }
};
class DecoderBoundary final : public VideoDecoder {
 public:
  explicit DecoderBoundary(std::shared_ptr<CodecBoundaryCounters> counters) : counters_(std::move(counters)) {}
  bool Configure(const Settings&) override { return true; }
  int32_t Decode(const EncodedImage&, int64_t) override {
    ++counters_->unexpected_video_decode_calls;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  int32_t RegisterDecodeCompleteCallback(DecodedImageCallback*) override { return WEBRTC_VIDEO_CODEC_OK; }
  int32_t Release() override { return WEBRTC_VIDEO_CODEC_OK; }
 private:
  std::shared_ptr<CodecBoundaryCounters> counters_;
};
class DecoderFactory final : public VideoDecoderFactory {
 public:
  explicit DecoderFactory(std::shared_ptr<CodecBoundaryCounters> counters) : counters_(std::move(counters)) {}
  std::vector<SdpVideoFormat> GetSupportedFormats() const override { return video_formats(); }
  std::unique_ptr<VideoDecoder> Create(const Environment&, const SdpVideoFormat& format) override {
    return format.IsCodecInList(video_formats()) ? std::make_unique<DecoderBoundary>(counters_) : nullptr;
  }
 private:
  std::shared_ptr<CodecBoundaryCounters> counters_;
};
} // namespace

EncodedVideoSender::EncodedVideoSender(BindingToken binding, TrackDescriptor descriptor)
    : webrtc::VideoTrackSource(false), state_(std::make_shared<VideoSenderState>()) {
  if (descriptor.kind != MediaKind::video || descriptor.encodings.size() != 1 ||
      native_codec(descriptor.format.codec) == webrtc::kVideoCodecGeneric ||
      !descriptor.format.width || !descriptor.format.height ||
      descriptor.format.width > 16384 || descriptor.format.height > 16384 ||
      descriptor.format.clock_rate != 90000 || !descriptor.queue.max_frames ||
      !descriptor.queue.max_bytes || descriptor.queue.max_age.count() <= 0)
    throw std::invalid_argument("Encoded video transport requires one encoding and a valid video descriptor");
  state_->binding = std::move(binding);
  state_->descriptor = std::move(descriptor);
  state_->muted = state_->descriptor.muted;
  state_->feedback.epoch = state_->binding.epoch;
  state_->feedback.revision = 1;
  auto& encoding = state_->feedback.encodings[state_->descriptor.encodings.front().id];
  encoding.keyframe_required = true;
  encoding.keyframe_requests = 1;
  encoding.target_bitrate_bps = state_->descriptor.encodings.front().max_bitrate_bps;
}
EncodedVideoSender::~EncodedVideoSender() { close(); }
SendResult EncodedVideoSender::send(const EncodedFrame& frame) {
  { std::lock_guard lock(state_->mutex); if (state_->closed || state_->muted || !state_->demanded) return SendResult::dropped; }
  if (!broadcaster_.frame_wanted()) return SendResult::would_block;
  auto packet = webrtc::make_ref_counted<PacketBuffer>(state_, frame);
  int64_t capture_us;
  {
    std::lock_guard lock(state_->mutex);
    const auto& descriptor = state_->descriptor;
    if (state_->closed || state_->muted || !state_->demanded || frame.epoch != state_->binding.epoch || frame.config_revision != 1 ||
        frame.encoding_id != descriptor.encodings.front().id || frame.buffer.size() == 0 ||
        frame.buffer.size() > descriptor.queue.max_bytes ||
        (state_->last_timestamp && frame.timestamp <= *state_->last_timestamp))
      return SendResult::dropped;
    if (state_->outstanding_frames >= descriptor.queue.max_frames ||
        frame.buffer.size() > descriptor.queue.max_bytes - state_->outstanding_bytes)
      return SendResult::would_block;
    // The native capture clock orders triggers; the explicit encoded RTP
    // timestamp preserves the external media clock without millisecond loss.
    capture_us = std::max(webrtc::Clock::GetRealTimeClock()->TimeInMicroseconds(), state_->last_capture_us + 1000);
    state_->last_capture_us = capture_us;
    state_->last_timestamp = frame.timestamp;
    ++state_->outstanding_frames;
    state_->outstanding_bytes += frame.buffer.size();
    packet->reserved = true;
    packet->generation = state_->generation;
  }
  auto native = webrtc::VideoFrame::Builder().set_video_frame_buffer(packet)
      .set_timestamp_us(capture_us).set_rotation(webrtc::kVideoRotation_0).build();
  native.set_encoded_rtp_timestamp(uint32_t(frame.timestamp));
  broadcaster_.OnFrame(native);
  return SendResult::sent;
}
SenderFeedback EncodedVideoSender::feedback() const {
  std::lock_guard lock(state_->mutex);
  auto feedback = state_->feedback;
  if (state_->muted || !state_->demanded) for (auto& [id, encoding] : feedback.encodings) encoding.active = false;
  return feedback;
}
void EncodedVideoSender::set_muted(bool muted) {
  std::lock_guard lock(state_->mutex);
  if (state_->closed || state_->muted == muted) return;
  state_->muted = muted; ++state_->generation;
  for (auto& [id, encoding] : state_->feedback.encodings) { encoding.keyframe_required = true; ++encoding.keyframe_requests; }
  ++state_->feedback.revision;
}
void EncodedVideoSender::close() {
  std::lock_guard lock(state_->mutex);
  state_->closed = true;
}
void EncodedVideoSender::set_demand(bool enabled) {
  std::lock_guard lock(state_->mutex);
  if (state_->closed || state_->demanded == enabled) return;
  state_->demanded = enabled; ++state_->generation;
  for (auto& [id, encoding] : state_->feedback.encodings) { encoding.keyframe_required = true; ++encoding.keyframe_requests; }
  ++state_->feedback.revision;
}
std::unique_ptr<webrtc::VideoEncoderFactory> create_transport_video_encoder_factory() {
  return std::make_unique<EncoderFactory>();
}
std::unique_ptr<webrtc::VideoDecoderFactory> create_transport_video_decoder_factory(
    std::shared_ptr<CodecBoundaryCounters> counters) {
  return std::make_unique<DecoderFactory>(std::move(counters));
}
} // namespace livekit::sdk::rtc::detail
