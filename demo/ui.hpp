#pragma once
#include "session.hpp"
#include "video_texture.hpp"
#include <array>

namespace demo {
enum class Action { None, Join, Leave };
class View {
 public:
  std::array<char, 1024> url{};
  std::array<char, 256> room{};
  std::array<char, 16384> token{};
  bool audio = true, video = false, tone = false;
  std::string error;
  View();
  Action draw(SDL_Renderer* renderer, const Snapshot& snapshot);
  sdk::RoomOptions options() const;
 private:
  struct Cached {
    VideoTexture texture;
    std::shared_ptr<const sdk::VideoFrame> last;
  };
  std::map<std::string, Cached> textures_;
};
void apply_style(float scale);
}
