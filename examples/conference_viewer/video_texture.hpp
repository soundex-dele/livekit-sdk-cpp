#pragma once
#include <livekit/media.hpp>
#include <SDL.h>

namespace demo {
class VideoTexture {
 public:
  ~VideoTexture();
  VideoTexture() = default;
  VideoTexture(const VideoTexture&) = delete;
  VideoTexture& operator=(const VideoTexture&) = delete;
  void upload(SDL_Renderer* renderer, const livekit::sdk::VideoFrame& frame);
  SDL_Texture* get() const { return texture_; }
  int width() const { return width_; }
  int height() const { return height_; }
 private:
  SDL_Texture* texture_ = nullptr;
  int width_ = 0, height_ = 0;
};
SDL_FRect fit_video(float x, float y, float width, float height, int video_width, int video_height);
}
