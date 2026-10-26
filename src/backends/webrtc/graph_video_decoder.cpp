#include "backends/webrtc/graph_video_decoder.hpp"
#include "media_graph/video_events.hpp"
#include "modules/video_coding/include/video_error_codes.h"

namespace livekit::sdk::detail {
namespace {
// Request/result references only cross synchronous edges on WebRTC's decode
// sequence. The backend may complete asynchronously via DecodedVideoFrame.
struct VideoDecodeRequest : graphflow::core::EventT<VideoDecodeRequest> {
  VideoDecodeRequest(const webrtc::EncodedImage& image, std::optional<bool> missing,
                     int64_t time, int32_t& result)
      : image(image), missing(missing), time(time), result(result) {}
  const webrtc::EncodedImage& image;
  std::optional<bool> missing;
  int64_t time;
  int32_t& result;
};
struct DecodedVideoFrame : graphflow::core::EventT<DecodedVideoFrame> {
  enum class Callback { frame, timed, optional };
  DecodedVideoFrame(webrtc::VideoFrame& frame, Callback kind, std::optional<int32_t> time,
                    std::optional<uint8_t> qp, int32_t& result)
      : frame(frame), kind(kind), time(time), qp(qp), result(result) {}
  webrtc::VideoFrame& frame;
  Callback kind;
  std::optional<int32_t> time;
  std::optional<uint8_t> qp;
  int64_t legacy_time = 0;
  int32_t& result;
};
class VideoDecoderNode final : public MediaNode, private webrtc::DecodedImageCallback {
 public:
  VideoDecoderNode(std::string format, std::shared_ptr<void> owner,
                   std::unique_ptr<webrtc::VideoDecoder> backend)
      : MediaNode("VideoDecoderNode", NodeExecution::backend, std::move(format)),
        owner_(std::move(owner)), backend_(std::move(backend)) {}
  ~VideoDecoderNode() override { release_codec(); }
  bool configure(const webrtc::VideoDecoder::Settings& settings) {
    ++generation; released_ = false;
    const bool ok = backend_->Configure(settings);
    if (!ok) ++errors;
    return ok;
  }
  int32_t register_callback(bool enabled) {
    return backend_->RegisterDecodeCompleteCallback(enabled ? this : nullptr);
  }
  int32_t release_codec() {
    if (released_) return WEBRTC_VIDEO_CODEC_OK;
    const auto result = backend_->Release(); // waits for outstanding callbacks
    released_ = true;
    return result;
  }
  webrtc::VideoDecoder& backend() { return *backend_; }
 protected:
  bool doInit() override {
    subscribe<VideoDecodeRequest>([this](VideoDecodeRequest& event) {
      ++inputs;
      event.result = event.missing ? backend_->Decode(event.image, *event.missing, event.time)
                                   : backend_->Decode(event.image, event.time);
      if (event.result < 0) ++errors;
    });
    return true;
  }
  void doStop() override { release_codec(); }
  void doRelease() override { release_codec(); }
 private:
  int32_t forward(webrtc::VideoFrame& frame, DecodedVideoFrame::Callback kind,
                  std::optional<int32_t> time = {}, std::optional<uint8_t> qp = {}) {
    ++outputs;
    int32_t result = WEBRTC_VIDEO_CODEC_OK;
    publish(DecodedVideoFrame(frame, kind, time, qp, result));
    return result;
  }
  int32_t Decoded(webrtc::VideoFrame& frame) override {
    return forward(frame, DecodedVideoFrame::Callback::frame);
  }
  int32_t Decoded(webrtc::VideoFrame& frame, int64_t time) override {
    // Preserve the full-width legacy decode time in its own synchronous path.
    ++outputs; int32_t result = WEBRTC_VIDEO_CODEC_OK;
    DecodedVideoFrame event(frame, DecodedVideoFrame::Callback::timed, {}, {}, result);
    event.legacy_time = time;
    publish(event); return result;
  }
  void Decoded(webrtc::VideoFrame& frame, std::optional<int32_t> time, std::optional<uint8_t> qp) override {
    forward(frame, DecodedVideoFrame::Callback::optional, time, qp);
  }
  std::shared_ptr<void> owner_;
  std::unique_ptr<webrtc::VideoDecoder> backend_;
  bool released_ = false;
};

class DecoderAdapter final : public webrtc::VideoDecoder {
 public:
  DecoderAdapter(std::shared_ptr<RtcGraph> graph, std::string format,
                 std::shared_ptr<void> owner, std::unique_ptr<webrtc::VideoDecoder> backend)
      : node_(std::make_shared<VideoDecoderNode>(std::move(format), std::move(owner), std::move(backend))),
        input_(std::make_shared<MediaEventNode<VideoDecodeRequest>>("RtcDecodeInputNode", NodeExecution::source)),
        output_(std::make_shared<MediaEventNode<DecodedVideoFrame>>("RtcDecodeOutputNode", NodeExecution::sink,
            [this](DecodedVideoFrame& event) {
              if (auto* cb = callback_.load()) {
                switch (event.kind) {
                  case DecodedVideoFrame::Callback::frame: event.result = cb->Decoded(event.frame); break;
                  case DecodedVideoFrame::Callback::timed: event.result = cb->Decoded(event.frame, event.legacy_time); break;
                  case DecodedVideoFrame::Callback::optional: cb->Decoded(event.frame, event.time, event.qp); break;
                }
              }
            })), pipeline_(std::move(graph)) {
    pipeline_.build({input_, node_, output_}, {
        RtcPipeline::connect<VideoDecodeRequest>(input_, node_),
        RtcPipeline::connect<DecodedVideoFrame>(node_, output_)});
    if (!pipeline_.start()) throw std::runtime_error("media_graph: decoder start failed");
  }
  ~DecoderAdapter() override { Release(); pipeline_.release(); }
  bool Configure(const Settings& settings) override { return node_->configure(settings); }
  int32_t Decode(const webrtc::EncodedImage& image, int64_t time) override {
    return decode(image, {}, time);
  }
  int32_t Decode(const webrtc::EncodedImage& image, bool missing, int64_t time) override {
    return decode(image, missing, time);
  }
  int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* cb) override {
    callback_.store(cb); return node_->register_callback(cb != nullptr);
  }
  int32_t Release() override {
    const auto result = node_->release_codec(); callback_.store(nullptr); return result;
  }
  DecoderInfo GetDecoderInfo() const override {
    auto info = node_->backend().GetDecoderInfo();
    info.implementation_name = "media_graph/" + info.implementation_name;
    return info;
  }
  const char* ImplementationName() const override { return node_->backend().ImplementationName(); }
 private:
  int32_t decode(const webrtc::EncodedImage& image, std::optional<bool> missing, int64_t time) {
    int32_t result = WEBRTC_VIDEO_CODEC_ERROR;
    input_->emit(VideoDecodeRequest(image, missing, time, result));
    return result;
  }
  std::atomic<webrtc::DecodedImageCallback*> callback_{nullptr};
  std::shared_ptr<VideoDecoderNode> node_;
  std::shared_ptr<MediaEventNode<VideoDecodeRequest>> input_;
  std::shared_ptr<MediaEventNode<DecodedVideoFrame>> output_;
  RtcPipeline pipeline_;
};
}
std::unique_ptr<webrtc::VideoDecoder> create_graph_video_decoder(
    std::shared_ptr<RtcGraph> graph, std::string format, std::shared_ptr<void> owner,
    std::unique_ptr<webrtc::VideoDecoder> backend) {
  return std::make_unique<DecoderAdapter>(std::move(graph), std::move(format), std::move(owner), std::move(backend));
}
}
