#include "engines/rtc/video_receive_graph.hpp"
#include "media_graph/video_events.hpp"
#include "media_nodes/render/video_render.hpp"
#include "media_nodes/recording/recording.hpp"
#include "modules/video_coding/include/video_error_codes.h"
#include "system_wrappers/include/clock.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <thread>

namespace livekit::sdk::detail {
namespace {
struct VideoDecodeRequest : graphflow::core::EventT<VideoDecodeRequest> {
  VideoDecodeRequest(webrtc::VideoDecoder& codec, const webrtc::EncodedImage& image,
                     std::optional<bool> missing, int64_t time, const std::string& format, int& result)
      : codec(codec), image(image), missing(missing), time(time), format(format), result(result) {}
  webrtc::VideoDecoder& codec;
  const webrtc::EncodedImage& image;
  std::optional<bool> missing;
  int64_t time;
  const std::string& format;
  int& result;
};
class ReceiveDecoderNode final : public MediaNode {
 public:
  ReceiveDecoderNode() : MediaNode("VideoDecoderNode", NodeExecution::backend) {}
  void decoded(const webrtc::VideoFrame& frame) { ++outputs; publish(RawVideoFrame(frame)); }
 protected:
  bool doInit() override {
    subscribe<VideoDecodeRequest>([this](auto& request) {
      ++inputs;
      set_format(request.format);
      request.result = request.missing ? request.codec.Decode(request.image, *request.missing, request.time)
                                       : request.codec.Decode(request.image, request.time);
      if (request.result < 0) ++errors;
    });
    return true;
  }
};
// Presentation uses the same local monotonic clock as the native RTC runtime.
// Only decoded frames may be evicted; compressed reference frames are not lost.
class VideoPresentNode final : public MediaNode {
 public:
  VideoPresentNode() : MediaNode("VideoPresentNode", NodeExecution::backend) {}
  ~VideoPresentNode() override { stop(); }
  std::string error() const { std::lock_guard lock(mutex_); return error_; }
 protected:
  bool doInit() override {
    subscribe<RawVideoFrame>([this](auto& event) {
      ++inputs;
      const auto bytes = static_cast<uint64_t>(event.frame.width()) * event.frame.height() * 4;
      const auto now = webrtc::Clock::GetRealTimeClock()->TimeInMicroseconds();
      std::lock_guard lock(mutex_);
      if (!running_ || bytes > max_bytes || event.frame.timestamp_us() > now + 10000000) { ++dropped; return; }
      while (!queue_.empty() && (queue_.size() >= 8 || bytes_ + bytes > max_bytes)) {
        bytes_ -= queue_.front().bytes; queue_.pop_front(); ++dropped;
      }
      auto pos = std::upper_bound(queue_.begin(), queue_.end(), event.frame.timestamp_us(),
          [](int64_t time, const Item& item) { return time < item.frame.timestamp_us(); });
      queue_.insert(pos, {event.frame, bytes}); bytes_ += bytes;
      ready_.notify_all();
    });
    return true;
  }
  bool doStart() override {
    { std::lock_guard lock(mutex_); running_ = true; error_.clear(); }
    worker_ = std::thread([this] { run(); });
    return true;
  }
  void doStop() override {
    { std::lock_guard lock(mutex_); running_ = false; queue_.clear(); bytes_ = 0; }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
  }
 private:
  struct Item { webrtc::VideoFrame frame; uint64_t bytes; };
  void run() {
    std::unique_lock lock(mutex_);
    while (running_) {
      if (queue_.empty()) { ready_.wait(lock, [&] { return !running_ || !queue_.empty(); }); continue; }
      const auto now = webrtc::Clock::GetRealTimeClock()->TimeInMicroseconds();
      const auto delay = queue_.front().frame.timestamp_us() - now;
      if (delay > 0) { ready_.wait_for(lock, std::chrono::microseconds(std::min<int64_t>(delay, 100000))); continue; }
      auto item = std::move(queue_.front()); queue_.pop_front(); bytes_ -= item.bytes;
      if (delay < -250000) { ++dropped; continue; }
      lock.unlock();
      try { publish(RawVideoFrame(item.frame)); ++outputs; }
      catch (const std::exception& error) { lock.lock(); error_ = error.what(); ++errors; continue; }
      lock.lock();
    }
  }
  static constexpr uint64_t max_bytes = 64 * 1024 * 1024;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Item> queue_;
  uint64_t bytes_ = 0;
  bool running_ = false;
  std::thread worker_;
  std::string error_;
};
}
struct VideoReceiveGraph::Impl {
  explicit Impl(std::shared_ptr<RtcGraph> registry) : pipeline(std::move(registry)) {}
  RtcPipeline pipeline;
  std::shared_ptr<MediaEventNode<VideoDecodeRequest>> source;
  std::shared_ptr<ReceiveDecoderNode> decoder;
  std::shared_ptr<VideoPresentNode> present;
  std::shared_ptr<lk::VideoRenderNode> renderer;
  std::shared_ptr<lk::RecordingNode> recorder;
  mutable std::mutex mutex;
  std::condition_variable idle;
  uint64_t generation = 0;
  size_t inflight = 0;
  bool running = false;
  std::string failure;
  // Close admission before waiting. Never hold this lock while calling a codec:
  // a synchronous codec callback is allowed to enter the same graph.
  struct Lease {
    Impl* owner = nullptr;
    Lease(Impl& impl, uint64_t generation) {
      std::lock_guard lock(impl.mutex);
      if (impl.running && impl.generation == generation) { owner = &impl; ++impl.inflight; }
    }
    ~Lease() { if (owner) { std::lock_guard lock(owner->mutex); if (!--owner->inflight) owner->idle.notify_all(); } }
  };
};
VideoReceiveGraph::VideoReceiveGraph(std::shared_ptr<RtcGraph> registry, std::string track_id,
    const std::filesystem::path& recording, lk::VideoPlayout* playout, lk::VideoFrames* frames)
    : impl_(std::make_unique<Impl>(std::move(registry))) {
  auto& p = *impl_;
  p.source = std::make_shared<MediaEventNode<VideoDecodeRequest>>("RtcEncodedSourceNode", NodeExecution::source);
  p.decoder = std::make_shared<ReceiveDecoderNode>();
  p.present = std::make_shared<VideoPresentNode>();
  p.renderer = std::make_shared<lk::VideoRenderNode>(track_id, playout, frames);
  std::vector<std::shared_ptr<MediaNode>> nodes{p.source, p.decoder, p.present, p.renderer};
  std::vector<RtcPipeline::Connection> edges{
      RtcPipeline::connect<VideoDecodeRequest>(p.source, p.decoder),
      RtcPipeline::connect<RawVideoFrame>(p.decoder, p.present),
      RtcPipeline::connect<RawVideoFrame>(p.present, p.renderer)};
  if (!recording.empty()) {
    p.recorder = std::make_shared<lk::RecordingNode>(recording, track_id, true);
    nodes.push_back(p.recorder);
    edges.push_back(RtcPipeline::connect<RawVideoFrame>(p.decoder, p.recorder));
  }
  p.pipeline.build(nodes, edges);
}
VideoReceiveGraph::~VideoReceiveGraph() { stop(); }
bool VideoReceiveGraph::start() {
  if (!impl_->pipeline.start()) return false;
  std::lock_guard lock(impl_->mutex);
  ++impl_->generation; impl_->running = true; impl_->failure.clear(); return true;
}
void VideoReceiveGraph::stop() {
  {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->running) return;
    impl_->running = false;
    impl_->idle.wait(lock, [&] { return impl_->inflight == 0; });
  }
  impl_->pipeline.stop();
}
uint64_t VideoReceiveGraph::generation() const { std::lock_guard lock(impl_->mutex); return impl_->generation; }
int VideoReceiveGraph::decode(uint64_t generation, webrtc::VideoDecoder& codec,
    const webrtc::EncodedImage& image, std::optional<bool> missing, int64_t time, const std::string& format) {
  Impl::Lease lease(*impl_, generation);
  if (!lease.owner) return WEBRTC_VIDEO_CODEC_ERROR;
  int result = WEBRTC_VIDEO_CODEC_ERROR;
  try { impl_->source->emit(VideoDecodeRequest(codec, image, missing, time, format, result)); }
  catch (const std::exception& error) { std::lock_guard lock(impl_->mutex); impl_->failure = error.what(); }
  return result;
}
void VideoReceiveGraph::decoded(uint64_t generation, const webrtc::VideoFrame& frame) {
  Impl::Lease lease(*impl_, generation);
  if (!lease.owner) return;
  try { impl_->decoder->decoded(frame); }
  catch (const std::exception& error) { std::lock_guard lock(impl_->mutex); impl_->failure = error.what(); }
}
uint64_t VideoReceiveGraph::frames() const { return impl_->decoder->outputs; }
std::string VideoReceiveGraph::error() const {
  { std::lock_guard lock(impl_->mutex); if (!impl_->failure.empty()) return impl_->failure; }
  auto error = impl_->present->error();
  return !error.empty() ? error : impl_->recorder ? impl_->recorder->error() : std::string{};
}
}
