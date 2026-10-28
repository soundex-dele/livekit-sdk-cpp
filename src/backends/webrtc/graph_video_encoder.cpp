#include "backends/webrtc/graph_video_encoder.hpp"
#include "media_graph/encode_executor.hpp"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "api/video/i420_buffer.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "modules/video_coding/svc/scalability_mode_util.h"
#include "system_wrappers/include/clock.h"
#include "rtc_base/logging.h"
#include <algorithm>
#include <deque>

namespace livekit::sdk::detail {
namespace {
using namespace webrtc;
struct EncoderConfig {
  VideoCodec codec;
  std::optional<VideoEncoder::Settings> settings;
  std::optional<VideoEncoder::RateControlParameters> rates;
  std::optional<VideoEncoder::LossNotification> loss_notification;
  float loss = 0;
  int64_t rtt = 0;
  uint64_t revision = 0, controls = 0, loss_revision = 0;
  bool active = false;
};
struct EncoderSession {
  EncoderSession(Environment env, SdpVideoFormat format, VideoCodecCreator create)
      : env(std::move(env)), format(std::move(format)), create(std::move(create)) {}
  EncoderConfig snapshot() { std::lock_guard lock(mutex); return config; }
  void request_keyframe() { ++recovery; }
  Environment env;
  SdpVideoFormat format;
  VideoCodecCreator create;
  std::mutex mutex;
  EncoderConfig config;
  std::atomic<uint64_t> recovery{1}, recovered{0};
};
struct Packet {
  EncodedImage image;
  CodecSpecificInfo info;
  uint64_t revision, recovery;
  std::vector<std::pair<EncodedImage,CodecSpecificInfo>> layers;
};
// The packet and its trigger travel together in one refcounted object. There
// is no separate FIFO (which would drift when WebRTC discards trigger frames),
// global encoder registry, raw encoder pointer, or black I420 allocation.
class PacketBuffer : public VideoFrameBuffer {
 public:
  using Bind = std::function<void(std::shared_ptr<EncoderSession>)>;
  PacketBuffer(int width, int height, Bind bind,
               std::shared_ptr<EncoderSession> session = {},
               std::optional<Packet> packet = {})
      : width_(width), height_(height), bind(std::move(bind)),
        session(std::move(session)), packet(std::move(packet)) {}
  ~PacketBuffer() override {
    // A trigger dropped by WebRTC must not leave a dependent delta-frame
    // chain in flight. Ask the actual graph encoder for a new keyframe.
    if (packet && !consumed && session) session->request_keyframe();
  }
  Type type() const override { return Type::kNative; }
  int width() const override { return width_; }
  int height() const override { return height_; }
  scoped_refptr<I420BufferInterface> ToI420() override { return nullptr; }
  Bind bind;
  std::shared_ptr<EncoderSession> session;
  std::optional<Packet> packet;
  std::atomic<bool> consumed{false};
 private:
  int width_, height_;
};

class ExternalPacketBuffer : public VideoFrameBuffer {
 public:
  ExternalPacketBuffer(EncodedMediaFrame value,std::shared_ptr<EncodedSourceReader> input):frame(std::move(value)),source(std::move(input)){}
  ~ExternalPacketBuffer(){if(!consumed)source->feedback(true);}
  Type type()const override{return Type::kNative;}
  int width()const override{return frame.width;}int height()const override{return frame.height;}
  scoped_refptr<I420BufferInterface> ToI420()override{return nullptr;}
  EncodedMediaFrame frame;std::shared_ptr<EncodedSourceReader> source;std::atomic<bool> consumed{false};
};
class PassthroughEncoder final : public VideoEncoder {
 public:
  PassthroughEncoder(const Environment& env, const SdpVideoFormat& format,
                     std::shared_ptr<RtcGraph> graph, VideoCodecCreator create)
      : session_(std::make_shared<EncoderSession>(env, format, std::move(create))),
        node_(graph->add(std::make_shared<MediaNode>("RtcPublishNode", NodeExecution::sink, format.name))) {
    node_->start();
  }
  ~PassthroughEncoder() override { Release(); node_->stop(); }
  int InitEncode(const VideoCodec* codec, const Settings& settings) override {
    if (!codec || (codec->numberOfSimulcastStreams > 1 && codec->codecType != kVideoCodecVP8) ||
        (codec->codecType == kVideoCodecVP9 && codec->VP9().numberOfSpatialLayers > 1) ||
        (codec->GetScalabilityMode() && ScalabilityModeToNumSpatialLayers(*codec->GetScalabilityMode()) > 1))
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    std::lock_guard lock(session_->mutex);
    session_->config.codec = *codec; session_->config.settings = settings;
    session_->config.loss_notification.reset();
    session_->config.active = true; ++session_->config.revision; ++node_->generation;
    session_->request_keyframe(); return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t InitEncode(const VideoCodec* codec, int32_t cores, size_t payload) override {
    return InitEncode(codec, Settings(Capabilities(false), cores, payload));
  }
  int32_t RegisterEncodeCompleteCallback(EncodedImageCallback* callback) override {
    callback_ = callback; return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Encode(const VideoFrame& frame, const std::vector<VideoFrameType>* types) override {
    if (frame.video_frame_buffer()->type() != VideoFrameBuffer::Type::kNative) {
      // VideoBroadcaster substitutes pixel-backed black frames when a track
      // is muted. Do not encode them here or forward queued source content.
      ++node_->dropped; session_->request_keyframe();
      if (callback_) callback_->OnDroppedFrame(EncodedImageCallback::DropReason::kDroppedByEncoder);
      return WEBRTC_VIDEO_CODEC_OK;
    }
    if(auto* external=dynamic_cast<ExternalPacketBuffer*>(frame.video_frame_buffer().get())){
      if(external->consumed.exchange(true))return WEBRTC_VIDEO_CODEC_OK;
      auto config=session_->snapshot();external_=external->source;
      std::string codec=session_->format.name;std::transform(codec.begin(),codec.end(),codec.begin(),[](unsigned char c){return char(std::tolower(c));});
      if(!config.active||!callback_||external->frame.codec!=codec||external->frame.width!=config.codec.width||external->frame.height!=config.codec.height)return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
      const bool requested=types&&std::find(types->begin(),types->end(),VideoFrameType::kVideoFrameKey)!=types->end();
      if((requested||session_->recovery>session_->recovered)&&!external->frame.keyframe){external->source->feedback(true);return WEBRTC_VIDEO_CODEC_OK;}
      EncodedImage image;image.SetEncodedData(EncodedImageBuffer::Create(external->frame.data.data(),external->frame.data.size()));
      image._encodedWidth=external->frame.width;image._encodedHeight=external->frame.height;image._frameType=external->frame.keyframe?VideoFrameType::kVideoFrameKey:VideoFrameType::kVideoFrameDelta;image.SetRtpTimestamp(frame.rtp_timestamp());
      CodecSpecificInfo info{};info.codecType=config.codec.codecType;
      if(info.codecType==kVideoCodecVP8){info.codecSpecific.VP8.temporalIdx=kNoTemporalIdx;info.codecSpecific.VP8.keyIdx=kNoKeyIdx;}
      if(info.codecType==kVideoCodecH264)info.codecSpecific.H264.packetization_mode=H264PacketizationMode::NonInterleaved;
      auto result=callback_->OnEncodedImage(image,&info);
      if(result.error!=EncodedImageCallback::Result::OK||result.drop_next_frame)external->source->feedback(true);
      else if(external->frame.keyframe)session_->recovered=session_->recovery.load();
      ++node_->inputs;++node_->outputs;return result.error==EncodedImageCallback::Result::OK?WEBRTC_VIDEO_CODEC_OK:WEBRTC_VIDEO_CODEC_ERROR;
    }
    auto* buffer = dynamic_cast<PacketBuffer*>(frame.video_frame_buffer().get());
    if (!buffer) { ++node_->errors; return WEBRTC_VIDEO_CODEC_ERR_PARAMETER; }
    if (buffer->consumed.exchange(true)) {
      // Screen-share cadence may repeat a native buffer with a new timestamp.
      // Replaying the same compressed reference picture is not a new encode.
      ++node_->dropped;
      if (types && std::find(types->begin(), types->end(), VideoFrameType::kVideoFrameKey) != types->end())
        session_->request_keyframe();
      if (callback_) callback_->OnDroppedFrame(EncodedImageCallback::DropReason::kDroppedByEncoder);
      return WEBRTC_VIDEO_CODEC_OK;
    }
    buffer->bind(session_); // explicit per-source binding carried by this frame
    auto config = session_->snapshot();
    if (!config.active || !callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    if (types && std::find(types->begin(), types->end(), VideoFrameType::kVideoFrameKey) != types->end() &&
        (!buffer->packet || buffer->packet->image._frameType != VideoFrameType::kVideoFrameKey))
      session_->request_keyframe();
    // A metadata-only probe obtains negotiated codec parameters before the
    // first real encode. Never transmit it as a fake picture.
    if (!buffer->packet) {
      callback_->OnDroppedFrame(EncodedImageCallback::DropReason::kDroppedByEncoder);
      return WEBRTC_VIDEO_CODEC_OK;
    }
    ++node_->inputs;
    if (buffer->session != session_ || buffer->packet->revision != config.revision ||
        (session_->recovery > session_->recovered && buffer->packet->image._frameType != VideoFrameType::kVideoFrameKey)) {
      ++node_->dropped; session_->request_keyframe();
      callback_->OnDroppedFrame(EncodedImageCallback::DropReason::kDroppedByEncoder);
      return WEBRTC_VIDEO_CODEC_OK;
    }
    for(const auto& layer:buffer->packet->layers){
      auto image=layer.first;image.SetRtpTimestamp(frame.rtp_timestamp());
      const auto result=callback_->OnEncodedImage(image,&layer.second);
      if(result.error!=EncodedImageCallback::Result::OK||result.drop_next_frame)session_->request_keyframe();
    }
    auto image = buffer->packet->image;
    // Use the exact timestamp of this trigger after WebRTC's clock mapping.
    image.SetRtpTimestamp(frame.rtp_timestamp());
    auto result = callback_->OnEncodedImage(image, &buffer->packet->info);
    if (result.error != EncodedImageCallback::Result::OK) {
      ++node_->errors; session_->request_keyframe(); return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (image._frameType == VideoFrameType::kVideoFrameKey) {
      auto previous = session_->recovered.load();
      while (previous < buffer->packet->recovery &&
             !session_->recovered.compare_exchange_weak(previous, buffer->packet->recovery)) {}
    }
    if (result.drop_next_frame) session_->request_keyframe();
    ++node_->outputs; return WEBRTC_VIDEO_CODEC_OK;
  }
  void SetRates(const RateControlParameters& rates) override {
    if(auto source=external_.lock())source->feedback(false,rates.bitrate.get_sum_bps());
    std::lock_guard lock(session_->mutex);
    if (!session_->config.rates || session_->config.rates->bitrate.get_sum_bps() == 0)
      session_->request_keyframe();
    session_->config.rates = rates; ++session_->config.controls;
  }
  void OnPacketLossRateUpdate(float loss) override {
    std::lock_guard lock(session_->mutex); session_->config.loss = loss; ++session_->config.controls;
  }
  void OnRttUpdate(int64_t rtt) override {
    std::lock_guard lock(session_->mutex); session_->config.rtt = rtt; ++session_->config.controls;
  }
  void OnLossNotification(const LossNotification& loss) override {
    std::lock_guard lock(session_->mutex); session_->config.loss_notification = loss;
    ++session_->config.controls; ++session_->config.loss_revision;
  }
  EncoderInfo GetEncoderInfo() const override {
    EncoderInfo info;
    info.implementation_name = "media_graph/passthrough";
    info.supports_native_handle = true;
    info.supports_simulcast = session_->format.name == "VP8";
    info.has_trusted_rate_controller = true;
    info.scaling_settings = ScalingSettings::kOff;
    info.is_hardware_accelerated = false;
    return info;
  }
  int32_t Release() override {
    std::lock_guard lock(session_->mutex);
    session_->config.active = false; callback_ = nullptr;
    session_->request_keyframe(); return WEBRTC_VIDEO_CODEC_OK;
  }
 private:
  std::weak_ptr<EncodedSourceReader> external_;
  std::shared_ptr<EncoderSession> session_;
  std::shared_ptr<MediaNode> node_;
  EncodedImageCallback* callback_ = nullptr; // stock WebRTC encoder sequence only
};
}

class VideoEncoderNode final : public MediaNode, public std::enable_shared_from_this<VideoEncoderNode>, private webrtc::EncodedImageCallback {
 public:
  VideoEncoderNode() : MediaNode("VideoEncoderNode", NodeExecution::backend),
      env_(webrtc::CreateEnvironment()), executor_(env_, "MediaGraphVideoEncoder", 1) {}
  ~VideoEncoderNode() override { shutdown(); }
  void submit(const webrtc::VideoFrame& frame) {
    std::lock_guard admission(admission_mutex_);
    if (paused_ || closing_) return;
    ++inputs;
    const auto epoch = epoch_.load();
    dropped += executor_.post([this, frame, epoch] {
      if (paused_ || epoch != epoch_) { ++dropped; return; }
      try { encode(frame); }
      catch (const std::exception& e) { ++errors; RTC_LOG(LS_ERROR) << "Graph encode: " << e.what(); }
    }, true);
  }
  void pause() {
    {
      std::lock_guard admission(admission_mutex_);
      paused_ = true; ++epoch_;
      executor_.post([this] { reset_encoder(); session_.reset(); });
    }
    executor_.flush();
  }
  void shutdown() {
    {
      std::lock_guard admission(admission_mutex_);
      if (closing_) return;
      closing_ = true; paused_ = true; ++epoch_;
      executor_.post([this] { reset_encoder(); session_.reset(); });
    }
    executor_.flush();
  }
  void resume() { std::lock_guard admission(admission_mutex_); if (!closing_) paused_ = false; }
 protected:
  bool doInit() override {
    subscribe<RawVideoFrame>([this](RawVideoFrame& event) { submit(event.frame); });
    return true;
  }
  bool doStart() override { resume(); return true; }
  void doStop() override { pause(); }
  void doRelease() override { shutdown(); }
 private:
  struct Metadata {
    uint32_t rtp;
    int width, height;
    int64_t time_us, ntp_ms;
    webrtc::VideoRotation rotation;
    std::optional<webrtc::ColorSpace> color;
    uint64_t recovery;
  };
  void reset_encoder() {
    if (encoder_) { encoder_->Release(); encoder_.reset(); }
    std::lock_guard lock(metadata_mutex_); metadata_.clear();
    revision_ = 0; controls_ = 0; last_frame_us_.reset(); next_frame_us_.reset();
  }
  PacketBuffer::Bind binding() {
    return [weak = weak_from_this()](std::shared_ptr<EncoderSession> session) {
      if (auto self = weak.lock()) {
        // Binding is a control message, never a codec invocation on WebRTC's
        // Encode() stack. Packet buffers hold only a weak source lease.
        std::lock_guard admission(self->admission_mutex_);
        if (self->closing_ || self->paused_) return;
        self->executor_.post([ptr = self.get(), session = std::move(session)] {
          if (!ptr->paused_ && ptr->session_ != session) {
            ptr->reset_encoder(); ptr->session_ = session;
          }
        });
      }
    };
  }
  void encode(webrtc::VideoFrame frame) {
    using namespace webrtc;
    if (frame.rotation() != kVideoRotation_0) {
      auto pixels = frame.video_frame_buffer()->ToI420();
      if (!pixels) { ++errors; return; }
      frame.set_video_frame_buffer(I420Buffer::Rotate(*pixels, frame.rotation()));
      frame.set_rotation(kVideoRotation_0);
    }
    const auto now = env_.clock().CurrentTime();
    const int64_t time_us = frame.timestamp_us() > 0 ? std::min(frame.timestamp_us(), now.us()) : now.us();
    frame.set_timestamp_us(time_us);
    if (frame.ntp_time_ms() <= 0)
      frame.set_ntp_time_ms(env_.clock().CurrentNtpInMilliseconds() + time_us / 1000 - now.ms());
    frame.set_rtp_timestamp(static_cast<uint32_t>(frame.ntp_time_ms()) * 90);
    auto config = session_ ? session_->snapshot() : EncoderConfig{};
    if (!config.active || !config.settings) {
      frame.set_video_frame_buffer(make_ref_counted<PacketBuffer>(frame.width(), frame.height(), binding()));
      publish(EncodedVideoFrame(frame)); return;
    }
    if (!encoder_ || revision_ != config.revision) {
      reset_encoder();
      encoder_ = session_->create(session_->env, session_->format);
      // The stock send pipeline sees a native packet envelope, but the actual
      // graph encoder receives CPU pixels, not a texture.
      config.codec.expect_encode_from_texture = false;
      if (!encoder_ || encoder_->InitEncode(&config.codec, *config.settings) != WEBRTC_VIDEO_CODEC_OK) {
        ++errors; reset_encoder(); return;
      }
      revision_ = config.revision;
      set_format(session_->format.name);
      encoder_->RegisterEncodeCompleteCallback(this);
      session_->request_keyframe(); ++generation;
    }
    if (controls_ != config.controls || !last_frame_us_) {
      if (config.rates) encoder_->SetRates(*config.rates);
      encoder_->OnPacketLossRateUpdate(config.loss); encoder_->OnRttUpdate(config.rtt);
      if (config.loss_notification && applied_loss_revision_ != config.loss_revision)
        encoder_->OnLossNotification(*config.loss_notification);
      applied_loss_revision_ = config.loss_revision;
      controls_ = config.controls;
    }
    if (config.rates && config.rates->bitrate.get_sum_bps() == 0) { ++dropped; return; }
    // RateControlParameters::framerate_fps is an observed input rate, not a
    // requested cap. Gating on it feeds our drops back into WebRTC's estimator
    // and progressively collapses a healthy 30 fps source to 1 fps.
    const double fps = config.codec.maxFramerate;
    if (fps <= 0 || (next_frame_us_ && time_us + 1000 < *next_frame_us_)) {
      ++dropped; return;
    }
    const int64_t period = static_cast<int64_t>(1000000 / fps);
    next_frame_us_ = next_frame_us_ && time_us - *next_frame_us_ < period
        ? *next_frame_us_ + period : time_us + period;
    last_frame_us_ = time_us;
    const auto recovery = session_->recovery.load();
    Metadata metadata{frame.rtp_timestamp(), frame.width(), frame.height(), time_us,
                      frame.ntp_time_ms(), frame.rotation(), frame.color_space(), recovery};
    if (frame.width() != config.codec.width || frame.height() != config.codec.height) {
      auto input = frame.video_frame_buffer()->ToI420();
      if (!input) { ++errors; return; }
      auto scaled = I420Buffer::Create(config.codec.width, config.codec.height);
      scaled->ScaleFrom(*input); frame.set_video_frame_buffer(scaled);
    }
    {
      std::lock_guard lock(metadata_mutex_);
      if (metadata_.size() == 64) { metadata_.pop_front(); session_->request_keyframe(); }
      metadata_.push_back(metadata);
    }
    std::vector<VideoFrameType> types(std::max<int>(1,config.codec.numberOfSimulcastStreams), recovery > session_->recovered
        ? VideoFrameType::kVideoFrameKey : VideoFrameType::kVideoFrameDelta);
    // Libvpx VP8 emits every simulcast layer synchronously from Encode().
    // Bundle them into one native trigger so cadence cannot discard sibling layers.
    batching_=config.codec.codecType==kVideoCodecVP8&&config.codec.numberOfSimulcastStreams>1;
    batch_.clear();
    auto result = encoder_->Encode(frame, &types);
    batching_=false;
    if(!batch_.empty()){auto last=std::move(batch_.back());batch_.pop_back();emit_encoded(last.first,&last.second,std::move(batch_));batch_.clear();}
    if (result < 0) { ++errors; session_->request_keyframe(); }
  }
  Result OnEncodedImage(const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo* info) override {
    if(batching_&&info){batch_.emplace_back(image,*info);return Result(Result::OK);}
    return emit_encoded(image,info,{});
  }
  Result emit_encoded(const webrtc::EncodedImage& image,const webrtc::CodecSpecificInfo* info,std::vector<std::pair<webrtc::EncodedImage,webrtc::CodecSpecificInfo>> layers){
    using namespace webrtc;
    if (paused_ || !info) return Result(Result::OK);
    std::optional<Metadata> metadata;
    {
      std::lock_guard lock(metadata_mutex_);
      auto it = std::find_if(metadata_.begin(), metadata_.end(), [&](const auto& m) { return m.rtp == image.RtpTimestamp(); });
      if (it != metadata_.end()) { metadata = *it; metadata_.erase(it); }
    }
    if (!metadata) { ++dropped; session_->request_keyframe(); return Result(Result::OK); }
    auto buffer = make_ref_counted<PacketBuffer>(metadata->width, metadata->height, binding(),
                                                session_, Packet{image, *info, revision_, metadata->recovery,std::move(layers)});
    auto frame = VideoFrame::Builder().set_video_frame_buffer(buffer)
        .set_timestamp_us(metadata->time_us).set_ntp_time_ms(metadata->ntp_ms)
        .set_rtp_timestamp(image.RtpTimestamp()).set_rotation(metadata->rotation)
        .set_color_space(metadata->color).build();
    ++outputs;
    publish(EncodedVideoFrame(frame));
    return Result(Result::OK);
  }
  void OnDroppedFrame(webrtc::EncodedImageCallback::DropReason) override { ++dropped; }
  webrtc::Environment env_;
  std::atomic<bool> paused_{true};
  std::mutex admission_mutex_;
  bool closing_ = false;
  std::atomic<uint64_t> epoch_{0};
  std::shared_ptr<EncoderSession> session_;
  std::unique_ptr<webrtc::VideoEncoder> encoder_;
  uint64_t revision_ = 0, controls_ = 0, applied_loss_revision_ = 0;
  std::optional<int64_t> last_frame_us_, next_frame_us_;
  std::mutex metadata_mutex_;
  std::deque<Metadata> metadata_;
  bool batching_=false;
  std::vector<std::pair<webrtc::EncodedImage,webrtc::CodecSpecificInfo>> batch_;
  EncodeExecutor executor_;
};
std::shared_ptr<MediaNode> create_video_encoder_node() {
  return std::make_shared<VideoEncoderNode>();
}
webrtc::VideoFrame external_encoded_video(EncodedMediaFrame frame,std::shared_ptr<EncodedSourceReader> source){
  const auto age=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-frame.captured).count();
  const auto time=webrtc::Clock::GetRealTimeClock()->TimeInMicroseconds()-age;
  auto buffer=webrtc::make_ref_counted<ExternalPacketBuffer>(std::move(frame),std::move(source));
  return webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_timestamp_us(time).build();
}
std::unique_ptr<webrtc::VideoEncoder> create_passthrough_video_encoder(
    const webrtc::Environment& env, const webrtc::SdpVideoFormat& format,
    std::shared_ptr<RtcGraph> graph, VideoCodecCreator create) {
  return std::make_unique<PassthroughEncoder>(env, format, std::move(graph), std::move(create));
}
}
