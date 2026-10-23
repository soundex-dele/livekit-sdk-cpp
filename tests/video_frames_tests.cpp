#include "lk/video_frames.hpp"
#include "api/video/i420_buffer.h"
#include <cstring>
#include <iostream>
#include <thread>

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
    std::cout << "Video frame tests passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
