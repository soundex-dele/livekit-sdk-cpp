#include "lk/video_playout.hpp"
#include "lk/auth.hpp"
#include "api/video/i420_buffer.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>

namespace lk {
namespace {
std::atomic_flag active_playout = ATOMIC_FLAG_INIT;
void check_sdl(int result, const char* action) {
  if (result < 0) throw std::runtime_error(std::string(action) + ": " + SDL_GetError());
}
struct Window {
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  SDL_Texture* texture = nullptr;
  int width = 0, height = 0;
  uint64_t presented = 0;
  bool dirty = true;
  ~Window() {
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
  }
};
}
struct VideoPlayout::Impl {
  EventEmitter event;
  std::mutex mutex;
  std::map<std::string, std::optional<webrtc::VideoFrame>> latest;
  // These objects, including their destructors, are main-thread-only.
  std::map<std::string, std::unique_ptr<Window>> windows;
  void summarize(const std::string& id, const Window& view) {
    event("video-playout-summary", {{"track", id}, {"frames", view.presented}});
  }
};
VideoPlayout::VideoPlayout(EventEmitter events) : impl_(std::make_unique<Impl>()) {
  impl_->event = std::move(events);
  if (active_playout.test_and_set()) throw std::runtime_error("Only one video playout session may run in a process");
  try {
    SDL_SetMainReady();
    check_sdl(SDL_InitSubSystem(SDL_INIT_VIDEO), "Cannot initialize video display");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  } catch (...) { active_playout.clear(); throw; }
}
VideoPlayout::~VideoPlayout() {
  for (const auto& [id, view] : impl_->windows) if (view) impl_->summarize(id, *view);
  impl_->windows.clear();
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  active_playout.clear();
}
void VideoPlayout::add_track(const std::string& id) {
  std::lock_guard lock(impl_->mutex);
  impl_->latest.try_emplace(id);
}
void VideoPlayout::remove_track(const std::string& id) {
  std::lock_guard lock(impl_->mutex);
  impl_->latest.erase(id);
}
void VideoPlayout::submit(const std::string& id, const webrtc::VideoFrame& frame) {
  std::lock_guard lock(impl_->mutex);
  auto found = impl_->latest.find(id);
  if (found != impl_->latest.end()) found->second = frame;
}
bool VideoPlayout::poll() {
  SDL_Event input;
  while (SDL_PollEvent(&input)) {
    if (input.type == SDL_QUIT ||
        (input.type == SDL_WINDOWEVENT && input.window.event == SDL_WINDOWEVENT_CLOSE) ||
        (input.type == SDL_KEYDOWN && input.key.keysym.sym == SDLK_ESCAPE)) {
      impl_->event("video-playout-closed"); return false;
    }
    if (input.type == SDL_WINDOWEVENT &&
        (input.window.event == SDL_WINDOWEVENT_EXPOSED || input.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
      for (auto& [id, view] : impl_->windows)
        if (SDL_GetWindowID(view->window) == input.window.windowID) view->dirty = true;
    }
  }
  std::map<std::string, webrtc::VideoFrame> frames;
  std::set<std::string> active;
  {
    std::lock_guard lock(impl_->mutex);
    for (auto& [id, frame] : impl_->latest) {
      active.insert(id);
      if (frame) { frames.emplace(id, std::move(*frame)); frame.reset(); }
    }
  }
  for (auto it = impl_->windows.begin(); it != impl_->windows.end();) {
    if (!active.contains(it->first)) {
      impl_->summarize(it->first, *it->second);
      it = impl_->windows.erase(it);
    } else ++it;
  }
  for (const auto& [id, frame] : frames) {
    auto buffer = frame.video_frame_buffer()->ToI420();
    if (!buffer) throw std::runtime_error("Display frame cannot convert to I420");
    if (frame.rotation() != webrtc::kVideoRotation_0)
      buffer = webrtc::I420Buffer::Rotate(*buffer, frame.rotation());
    const int width = buffer->width(), height = buffer->height();
    auto& view = impl_->windows[id];
    if (!view) {
      auto created = std::make_unique<Window>();
      double scale = std::min({1.0, 960.0 / width, 720.0 / height});
      created->window = SDL_CreateWindow(("LiveKit - " + id).c_str(),
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        std::max(1, static_cast<int>(width * scale)), std::max(1, static_cast<int>(height * scale)),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
      if (!created->window) throw std::runtime_error(std::string("Cannot create video window: ") + SDL_GetError());
      // No vsync wait on the signaling/main loop. Software rendering is a
      // fallback for remote desktop sessions and systems without a GPU driver.
      created->renderer = SDL_CreateRenderer(created->window, -1, SDL_RENDERER_ACCELERATED);
      if (!created->renderer) created->renderer = SDL_CreateRenderer(created->window, -1, SDL_RENDERER_SOFTWARE);
      if (!created->renderer) throw std::runtime_error(std::string("Cannot create video renderer: ") + SDL_GetError());
      SDL_RendererInfo info{}; SDL_GetRendererInfo(created->renderer, &info);
      impl_->event("video-window", {{"track", id}, {"windowId", SDL_GetWindowID(created->window)},
                            {"renderer", info.name ? info.name : "unknown"}});
      view = std::move(created);
    }
    if (!view->texture || view->width != width || view->height != height) {
      auto texture = SDL_CreateTexture(view->renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
      if (!texture) throw std::runtime_error(std::string("Cannot create YUV texture: ") + SDL_GetError());
      if (view->texture) SDL_DestroyTexture(view->texture);
      view->texture = texture; view->width = width; view->height = height;
      check_sdl(SDL_RenderSetLogicalSize(view->renderer, width, height), "Cannot set video aspect ratio");
      impl_->event("video-format", {{"track", id}, {"width", width}, {"height", height}});
    }
    check_sdl(SDL_UpdateYUVTexture(view->texture, nullptr,
      buffer->DataY(), buffer->StrideY(), buffer->DataU(), buffer->StrideU(), buffer->DataV(), buffer->StrideV()), "Cannot upload video frame");
    ++view->presented;
    view->dirty = true;
  }
  // Repaint resize/expose events even without a new frame. Otherwise leave the
  // last image on screen instead of redrawing at the signaling poll frequency.
  for (const auto& [id, view] : impl_->windows) {
    if (!view->dirty) continue;
    check_sdl(SDL_SetRenderDrawColor(view->renderer, 0, 0, 0, 255), "Cannot set background");
    check_sdl(SDL_RenderClear(view->renderer), "Cannot clear video window");
    check_sdl(SDL_RenderCopy(view->renderer, view->texture, nullptr, nullptr), "Cannot render video frame");
    SDL_RenderPresent(view->renderer);
    view->dirty = false;
  }
  return true;
}
}
