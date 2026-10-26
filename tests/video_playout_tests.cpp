#include "media_nodes/render/video_playout.hpp"
#include "api/video/i420_buffer.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static webrtc::VideoFrame frame(int width, int height, webrtc::VideoRotation rotation = webrtc::kVideoRotation_0) {
  // Deliberately padded planes and odd dimensions catch pitch/chroma mistakes.
  auto buffer = webrtc::I420Buffer::Create(width, height, width + 17, (width + 1) / 2 + 9, (width + 1) / 2 + 11);
  std::memset(buffer->MutableDataY(), 235, buffer->StrideY() * height);
  std::memset(buffer->MutableDataU(), 128, buffer->StrideU() * ((height + 1) / 2));
  std::memset(buffer->MutableDataV(), 128, buffer->StrideV() * ((height + 1) / 2));
  return webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_rotation(rotation).set_timestamp_us(1).build();
}
int main() {
  try {
    {
      lk::VideoPlayout display;
      bool duplicate_rejected = false;
      try { lk::VideoPlayout duplicate; } catch (const std::runtime_error&) { duplicate_rejected = true; }
      check(duplicate_rejected, "reject concurrent SDL event owners");
      display.add_track("first");
      // Decode thread can overwrite pending frames without blocking on SDL.
      std::thread producer([&] {
        for (int i = 0; i < 100; ++i) display.submit("first", frame(64, 48));
        display.submit("first", frame(320, 180));
      });
      producer.join(); check(display.poll(), "initial rendering");
      auto window = SDL_GetWindowFromID(1); check(window != nullptr, "video window created");
      auto renderer = SDL_GetRenderer(window); check(renderer != nullptr, "SDL renderer exists");
      int width, height; SDL_RenderGetLogicalSize(renderer, &width, &height);
      check(width == 320 && height == 180, "latest queued frame wins");
      // Software backend permits readback of the rendered pixels.
      int output_width, output_height; SDL_GetRendererOutputSize(renderer, &output_width, &output_height);
      std::vector<uint32_t> pixels(output_width * output_height);
      check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, pixels.data(), output_width * 4) == 0, "render readback");
      auto center = pixels[(output_height / 2) * output_width + output_width / 2];
      check((center & 0xff) > 220 && ((center >> 8) & 0xff) > 220 && ((center >> 16) & 0xff) > 220, "YUV white frame reaches renderer");
      display.submit("first", frame(321, 181, webrtc::kVideoRotation_90));
      check(display.poll(), "resolution and rotation change");
      SDL_RenderGetLogicalSize(renderer, &width, &height);
      check(width == 181 && height == 321, "rotation changes display dimensions");
      SDL_SetWindowSize(window, 500, 500); check(display.poll(), "repaint on resize without another frame");
      SDL_RenderGetLogicalSize(renderer, &width, &height);
      check(width == 181 && height == 321, "resize preserves video aspect ratio");
      display.add_track("second"); display.submit("second", frame(80, 60));
      check(display.poll() && SDL_GetWindowFromID(2), "independent second track window");
      display.remove_track("second"); display.submit("second", frame(80, 60));
      check(display.poll() && !SDL_GetWindowFromID(2), "unsubscribed track closes and ignores late frames");
      SDL_Event close{}; close.type = SDL_WINDOWEVENT;
      close.window.windowID = SDL_GetWindowID(window); close.window.event = SDL_WINDOWEVENT_CLOSE;
      check(SDL_PushEvent(&close) == 1, "queue window close");
      check(!display.poll(), "window close requests orderly room exit");
    }
    check(SDL_WasInit(SDL_INIT_VIDEO) == 0, "SDL video shuts down");
    std::cout << "video playout tests passed\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
