#pragma once
#include "livekit/player.hpp"
#include <SDL.h>
#include <functional>

namespace player_demo {
namespace sdk = livekit::sdk;
enum class Operation { none, open, play, pause, seek, stop, close };

// UI-thread adapter. Futures are polled, never waited on in the event loop.
// SDL devices and textures belong here; SDK media objects remain independent.
class Session {
 public:
  explicit Session(SDL_Renderer* renderer) : renderer_(renderer) {}
  ~Session();
  void open(const std::string& path);
  void toggle_play();
  void pause(std::function<void()> after = {});
  void seek(int64_t position);
  void stop();
  void close();
  void update();
  bool busy() const { return operation_ != Operation::none; }
  bool closing() const { return closing_; }
  bool finished() const { return closing_ && !busy(); }
  bool drained() const;
  sdk::PlayerSnapshot snapshot() const { return player_.snapshot(); }
  Operation operation() const { return operation_; }
  SDL_Texture* texture() const { return texture_; }
  int video_width() const { return width_; }
  int video_height() const { return height_; }
  const std::string& path() const { return path_; }
  const std::string& error() const { return error_; }
  const std::string& warning() const { return warning_; }
  void clear_error() { error_.clear(); }
  void set_error(std::string text) { error_ = std::move(text); }
  float volume = .8f;
  bool muted = false;
 private:
  void command(Operation kind, std::future<void> future);
  void reset_output(bool clear_texture);
  void create_audio();
  void upload(const sdk::media::VideoFrame& frame);
  sdk::Player player_;
  SDL_Renderer* renderer_;
  SDL_Texture* texture_ = nullptr;
  SDL_AudioDeviceID audio_ = 0;
  int width_ = 0, height_ = 0;
  bool audio_started_ = false, closing_ = false;
  uint64_t generation_ = 0;
  Operation operation_ = Operation::none;
  std::future<sdk::PlayerMediaInfo> opening_;
  std::future<void> pending_;
  std::function<void()> after_;
  std::string path_, opening_path_, error_, warning_;
  std::vector<float> audio_buffer_;
};
}
