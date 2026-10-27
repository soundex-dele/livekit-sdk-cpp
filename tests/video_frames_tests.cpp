#include "media_nodes/render/video_frames.hpp"
#include "api/video/i420_buffer.h"
#include <cstring>
#include <iostream>
#include <thread>
#include <future>
#include "api/make_ref_counted.h"

class BlockingBuffer : public webrtc::VideoFrameBuffer {
 public:
  BlockingBuffer(std::promise<void>& entered, std::shared_future<void> release)
      : entered_(entered), release_(std::move(release)), buffer_(webrtc::I420Buffer::Create(64, 48)) {}
  Type type() const override { return Type::kNative; }
  int width() const override { return 64; }
  int height() const override { return 48; }
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
    entered_.set_value(); release_.wait(); return buffer_;
  }
 private:
  std::promise<void>& entered_;
  std::shared_future<void> release_;
  webrtc::scoped_refptr<webrtc::I420Buffer> buffer_;
};

static void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
static webrtc::VideoFrame frame(int w, int h, int64_t stamp, webrtc::VideoRotation rotation = webrtc::kVideoRotation_0) {
  auto b = webrtc::I420Buffer::Create(w, h, w + 7, (w + 1) / 2 + 9, (w + 1) / 2 + 11);
  std::memset(b->MutableDataY(), 81, b->StrideY() * h);
  std::memset(b->MutableDataU(), 90, b->StrideU() * ((h + 1) / 2));
  std::memset(b->MutableDataV(), 240, b->StrideV() * ((h + 1) / 2));
  b->MutableDataY()[0] = 16;
  b->MutableDataY()[w - 1] = 64;
  b->MutableDataY()[(h - 1) * b->StrideY()] = 128;
  b->MutableDataY()[(h - 1) * b->StrideY() + w - 1] = 235;
  return webrtc::VideoFrame::Builder().set_video_frame_buffer(b).set_timestamp_us(stamp).set_rotation(rotation).build();
}
int main() {
  try {
    int calls = 0; livekit::sdk::VideoFrame saved; const auto caller = std::this_thread::get_id();
    lk::VideoFrames queue([&](const auto& f) { check(caller == std::this_thread::get_id(), "callback thread"); ++calls; saved = f; });
    queue.add("one"); queue.add("two");
    std::thread producer([&] { for (int i = 0; i < 100; ++i) queue.submit("one", frame(321, 181, i)); }); producer.join();
    check(calls == 0, "producer invoked application"); queue.dispatch();
    check(calls == 1 && saved.timestamp_us == 99 && saved.width == 321, "latest frame must win");
    check(saved.y.size() == 321 * 181 && saved.u.size() == 161 * 91 && saved.v.back() == 240, "stride or odd chroma copy");
    queue.dispatch(); check(calls == 1, "frame redelivered");
    for (auto rotation : {webrtc::kVideoRotation_90, webrtc::kVideoRotation_180, webrtc::kVideoRotation_270}) {
      queue.submit("one", frame(321, 181, 200, rotation)); queue.dispatch();
      check(saved.width == (rotation == webrtc::kVideoRotation_180 ? 321 : 181), "rotation width");
      check(saved.height == (rotation == webrtc::kVideoRotation_180 ? 181 : 321), "rotation height");
      check(saved.y.front() == (rotation == webrtc::kVideoRotation_90 ? 128 : rotation == webrtc::kVideoRotation_180 ? 235 : 64), "rotation pixel orientation");
    }
    queue.submit("two", frame(80, 60, 201)); queue.remove("two"); queue.submit("two", frame(80, 60, 202));
    queue.dispatch(); check(calls == 4, "late removed frame delivered");
    queue.remove("one"); queue.add("one"); queue.dispatch(); check(calls == 4, "reuse leaked pending frame");
    queue.submit("one", frame(64, 48, 203)); queue.dispatch(); check(saved.width == 64, "resolution change");
    bool failed = false;
    lk::VideoFrames throwing([](const auto&) { throw std::runtime_error("expected"); });
    throwing.add("one"); throwing.submit("one", frame(64, 48, 1));
    try { throwing.dispatch(); } catch (...) { failed = true; } check(failed, "callback exception swallowed");
    using namespace livekit::sdk;
    using namespace std::chrono_literals;
    auto stream = std::make_shared<VideoFrameStream>(VideoStreamOptions{2, 10000});
    auto output = std::make_shared<detail::VideoFrameOutput>(stream); output->advance();
    lk::VideoFrames pull({}, output); pull.add("a"); pull.add("b");
    for (int i = 0; i < 20; ++i) { pull.submit("a", frame(64, 48, i)); pull.dispatch(); }
    auto stats = stream->stats();
    check(stats.received == 20 && stats.queued_frames == 2 && stats.queued_bytes == 9216 && stats.dropped == 18, "pull queue is not bounded");
    auto retained = stream->read(0ms);
    check(retained.status == VideoReadStatus::frame && retained.frame->timestamp_us == 18 && retained.frame->stream_generation == 1,
        "pull frame order/generation");
    const auto retained_pixel = retained.frame->y.front();
    pull.reset();
    check(stream->read(0ms).status == VideoReadStatus::timeout && stream->stats().generation == 2, "old generation survived reset");
    pull.submit("b", frame(64, 48, 30)); pull.dispatch(); pull.remove("b");
    check(stream->read(0ms).status == VideoReadStatus::timeout, "removed track remains queued");
    pull.submit("a", frame(640, 360, 31)); pull.dispatch();
    check(stream->stats().queued_bytes == 0 && stream->stats().dropped == 21, "oversized frame was retained");
    pull.submit("a", frame(64, 48, 32)); pull.dispatch();
    std::stop_source stopped; stopped.request_stop();
    check(stream->read(0ms, stopped.get_token()).status == VideoReadStatus::cancelled && stream->stats().queued_frames == 1,
        "cancelled reader consumed a frame");
    check(stream->read(0ms).frame->stream_generation == 2, "new transport generation missing");
    std::stop_source read_stop;
    auto cancelled_read = std::async(std::launch::async, [&] { return stream->read(10s, read_stop.get_token()); });
    read_stop.request_stop();
    check(cancelled_read.wait_for(1s) == std::future_status::ready && cancelled_read.get().status == VideoReadStatus::cancelled, "read cancellation did not wake");
    auto closed_read = std::async(std::launch::async, [&] { return stream->read(10s); });
    stream->close();
    check(closed_read.wait_for(1s) == std::future_status::ready && closed_read.get().status == VideoReadStatus::closed, "close did not wake reader");
    pull.submit("a", frame(64, 48, 33)); pull.dispatch();
    check(stream->stats().queued_frames == 0 && retained.frame->y.front() == retained_pixel, "closed stream accepted data or invalidated owned frame");
    failed = false; try { detail::VideoFrameOutput duplicate(stream); } catch (const std::invalid_argument&) { failed = true; }
    check(failed, "stream reused across sessions");
    auto byte_stream = std::make_shared<VideoFrameStream>(VideoStreamOptions{8, 6000});
    auto byte_output = std::make_shared<detail::VideoFrameOutput>(byte_stream); byte_output->advance();
    lk::VideoFrames byte_pull({}, byte_output); byte_pull.add("one");
    for (int i = 0; i < 3; ++i) { byte_pull.submit("one", frame(64, 48, i)); byte_pull.dispatch(); }
    check(byte_stream->stats().queued_frames == 1 && byte_stream->stats().dropped == 2, "byte cap did not evict below frame cap");
    byte_stream->read(0ms);
    // A native conversion can be in flight across remove/re-add or migration.
    // It must not be tagged as output of the new incarnation/generation.
    for (bool reset : {false, true}) {
      std::promise<void> entered, release;
      auto entered_future = entered.get_future();
      auto buffer = webrtc::make_ref_counted<BlockingBuffer>(entered, release.get_future().share());
      byte_pull.submit("one", webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).build());
      auto dispatch = std::async(std::launch::async, [&] { byte_pull.dispatch(); });
      const bool converting = entered_future.wait_for(2s) == std::future_status::ready;
      if (reset) byte_pull.reset(); else { byte_pull.remove("one"); byte_pull.add("one"); }
      release.set_value(); dispatch.get();
      check(converting && byte_stream->read(0ms).status == VideoReadStatus::timeout, "in-flight old frame crossed generation");
    }
    std::cout << "Video frame tests passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
