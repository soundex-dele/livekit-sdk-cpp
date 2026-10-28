#include "backends/webrtc/graph_video_decoder.hpp"
#include "media_graph/video_decode_target.hpp"
#include "modules/video_coding/include/video_error_codes.h"
#include "system_wrappers/include/clock.h"
#include <map>

namespace livekit::sdk::detail {
namespace {
// Standard DecoderFactory adapter: WebRTC schedules Decode(), but its decoded
// callback is never called. The codec is leased here and executed by the graph.
class DecoderAdapter final : public webrtc::VideoDecoder, private webrtc::DecodedImageCallback {
 public:
  DecoderAdapter(std::shared_ptr<RtcGraph> graph, std::string format,
                 std::shared_ptr<void> owner, std::unique_ptr<webrtc::VideoDecoder> backend)
      : graph_(std::move(graph)), format_(std::move(format)), owner_(std::move(owner)), backend_(std::move(backend)) {}
  ~DecoderAdapter() override { Release(); }
  bool Configure(const Settings& settings) override {
    if (!released_) Release();
    released_ = false;
    if (!backend_->Configure(settings)) return false;
    return backend_->RegisterDecodeCompleteCallback(enabled_ ? this : nullptr) == WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Decode(const webrtc::EncodedImage& image, int64_t time) override { return decode(image, {}, time); }
  int32_t Decode(const webrtc::EncodedImage& image, bool missing, int64_t time) override { return decode(image, missing, time); }
  int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
    // Only an enable/disable signal. No decoded pixels go back into WebRTC.
    enabled_ = callback != nullptr;
    return backend_->RegisterDecodeCompleteCallback(enabled_ ? this : nullptr);
  }
  int32_t Release() override {
    if (released_) return WEBRTC_VIDEO_CODEC_OK;
    const auto result = backend_->Release(); // joins outstanding codec callbacks
    released_ = true;
    std::lock_guard lock(mutex_); pending_.clear();
    return result;
  }
  DecoderInfo GetDecoderInfo() const override {
    auto info = backend_->GetDecoderInfo();
    info.implementation_name = "media_graph/direct/" + info.implementation_name;
    return info;
  }
  const char* ImplementationName() const override { return backend_->ImplementationName(); }
 private:
  struct Metadata {
    std::weak_ptr<VideoDecodeTarget> route;
    uint64_t generation;
    int64_t render_us;
    int64_t ntp_ms;
    webrtc::VideoRotation rotation;
    webrtc::RtpPacketInfos packets;
    webrtc::Timestamp decode_start;
  };
  int32_t decode(const webrtc::EncodedImage& image, std::optional<bool> missing, int64_t time) {
    if (released_ || image.PacketInfos().empty()) return WEBRTC_VIDEO_CODEC_ERROR;
    const auto ssrc = image.PacketInfos().front().ssrc();
    for (const auto& packet : image.PacketInfos()) if (packet.ssrc() != ssrc) return WEBRTC_VIDEO_CODEC_ERROR;
    auto route = graph_->video_route(ssrc);
    if (!route) return WEBRTC_VIDEO_CODEC_ERROR; // never guess by codec or creation order
    const auto generation = route->generation();
    const auto now = webrtc::Clock::GetRealTimeClock()->CurrentTime();
    {
      std::lock_guard lock(mutex_);
      // Bound metadata for codecs that fail to complete; the error asks WebRTC
      // to recover the reference chain rather than accumulate infinite state.
      if (pending_.size() >= 64) { pending_.clear(); return WEBRTC_VIDEO_CODEC_ERROR; }
      if (pending_.contains(image.RtpTimestamp())) return WEBRTC_VIDEO_CODEC_ERROR;
      pending_.emplace(image.RtpTimestamp(), Metadata{route, generation,
          time > 0 ? time * 1000 : now.us(), image.ntp_time_ms_, image.rotation(), image.PacketInfos(), now});
    }
    const auto result = route->decode(generation, *backend_, image, missing, time, format_);
    if (result < 0 || result == WEBRTC_VIDEO_CODEC_NO_OUTPUT) {
      std::lock_guard lock(mutex_); pending_.erase(image.RtpTimestamp());
    }
    return route->keyframe_requested.exchange(false)&&result>=0?WEBRTC_VIDEO_CODEC_OK_REQUEST_KEYFRAME:result;
  }
  void output(webrtc::VideoFrame& decoded) {
    std::optional<Metadata> metadata;
    {
      std::lock_guard lock(mutex_);
      auto it = pending_.find(decoded.rtp_timestamp());
      if (it == pending_.end()) return;
      metadata = std::move(it->second); pending_.erase(it);
    }
    if (auto route = metadata->route.lock(); route && enabled_) {
      auto frame = decoded; // share native pixels, restore timing/rotation ourselves
      frame.set_timestamp_us(metadata->render_us);
      frame.set_ntp_time_ms(metadata->ntp_ms);
      frame.set_rotation(metadata->rotation);
      frame.set_packet_infos(metadata->packets);
      frame.set_processing_time({metadata->decode_start, webrtc::Clock::GetRealTimeClock()->CurrentTime()});
      route->decoded(metadata->generation, frame);
    }
  }
  int32_t Decoded(webrtc::VideoFrame& frame) override { output(frame); return WEBRTC_VIDEO_CODEC_OK; }
  int32_t Decoded(webrtc::VideoFrame& frame, int64_t) override { return Decoded(frame); }
  void Decoded(webrtc::VideoFrame& frame, std::optional<int32_t>, std::optional<uint8_t>) override { output(frame); }
  std::shared_ptr<RtcGraph> graph_;
  std::string format_;
  std::shared_ptr<void> owner_; // delegate factory outlives the native codec
  std::unique_ptr<webrtc::VideoDecoder> backend_;
  bool released_ = true;
  std::atomic<bool> enabled_{false};
  std::mutex mutex_;
  std::map<uint32_t, Metadata> pending_;
};
}
std::unique_ptr<webrtc::VideoDecoder> create_graph_video_decoder(
    std::shared_ptr<RtcGraph> graph, std::string format, std::shared_ptr<void> owner,
    std::unique_ptr<webrtc::VideoDecoder> backend) {
  return std::make_unique<DecoderAdapter>(std::move(graph), std::move(format), std::move(owner), std::move(backend));
}
}
