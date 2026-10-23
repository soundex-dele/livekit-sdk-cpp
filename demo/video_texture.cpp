#include "video_texture.hpp"
#include <algorithm>
#include <stdexcept>

namespace demo {
VideoTexture::~VideoTexture() { if (texture_) SDL_DestroyTexture(texture_); }
void VideoTexture::upload(SDL_Renderer* renderer, const livekit::sdk::VideoFrame& frame) {
  if (frame.width <= 0 || frame.height <= 0 || frame.width > 16384 || frame.height > 16384)
    throw std::invalid_argument("Invalid I420 dimensions");
  const int cw = (frame.width + 1) / 2, ch = (frame.height + 1) / 2;
  auto valid = [](const auto& plane, int pitch, int w, int h) { return pitch >= w && plane.size() >= static_cast<size_t>(pitch) * h; };
  if (!valid(frame.y, frame.stride_y, frame.width, frame.height) || !valid(frame.u, frame.stride_u, cw, ch) || !valid(frame.v, frame.stride_v, cw, ch))
    throw std::invalid_argument("Invalid I420 frame");
  if (!texture_ || width_ != frame.width || height_ != frame.height) {
    auto* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
    if (!texture) throw std::runtime_error(SDL_GetError());
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = texture; width_ = frame.width; height_ = frame.height;
  }
  if (SDL_UpdateYUVTexture(texture_, nullptr, frame.y.data(), frame.stride_y, frame.u.data(), frame.stride_u, frame.v.data(), frame.stride_v) < 0)
    throw std::runtime_error(SDL_GetError());
}
SDL_FRect fit_video(float x, float y, float width, float height, int vw, int vh) {
  const float scale = std::min(width / vw, height / vh);
  return {x + (width - vw * scale) / 2, y + (height - vh * scale) / 2, vw * scale, vh * scale};
}
}
