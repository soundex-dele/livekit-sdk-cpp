#include "session.hpp"
#include <algorithm>

namespace player_demo {
using namespace std::chrono_literals;
namespace {
void check_sdl(bool ok) { if (!ok) throw std::runtime_error(SDL_GetError()); }
}
Session::~Session() {
  player_.cancel_pending_io();
  if (audio_) SDL_CloseAudioDevice(audio_);
  SDL_DestroyTexture(texture_);
}
void Session::reset_output(bool clear_texture) {
  if (audio_) { SDL_PauseAudioDevice(audio_, 1); SDL_ClearQueuedAudio(audio_); }
  audio_started_ = false;
  if (clear_texture) { SDL_DestroyTexture(texture_); texture_ = nullptr; width_ = height_ = 0; }
}
void Session::command(Operation kind, std::future<void> future) {
  operation_ = kind; pending_ = std::move(future);
}
void Session::open(const std::string& path) {
  if (busy() || closing_ || path.empty()) return;
  reset_output(true);
  if (audio_) { SDL_CloseAudioDevice(audio_); audio_ = 0; }
  error_.clear(); warning_.clear();
  opening_path_ = path;
  opening_ = player_.open(path); operation_ = Operation::open;
}
void Session::toggle_play() {
  if (busy() || closing_) return;
  const auto state = snapshot().state;
  if (state == sdk::PlayerState::playing) { pause(); return; }
  if (state == sdk::PlayerState::closed || state == sdk::PlayerState::failed) return;
  if (state == sdk::PlayerState::ended) reset_output(true);
  command(Operation::play, player_.play());
}
void Session::pause(std::function<void()> after) {
  if (busy() || closing_) return;
  if (snapshot().state != sdk::PlayerState::playing) { if (after) after(); return; }
  if (audio_) SDL_PauseAudioDevice(audio_, 1);
  after_ = std::move(after);
  command(Operation::pause, player_.pause());
}
void Session::seek(int64_t position) {
  if (busy() || closing_) return;
  auto state = snapshot();
  if (state.state == sdk::PlayerState::closed || state.state == sdk::PlayerState::failed) return;
  position = std::max<int64_t>(0, position);
  if (state.media.duration_us > 0) position = std::min(position, state.media.duration_us);
  reset_output(true);
  command(Operation::seek, player_.seek(std::chrono::microseconds(position)));
}
void Session::stop() {
  if (busy() || closing_) return;
  auto state = snapshot().state;
  if (state == sdk::PlayerState::closed || state == sdk::PlayerState::failed) return;
  reset_output(true);
  command(Operation::stop, player_.stop());
}
void Session::close() {
  if (closing_) return;
  closing_ = true; after_ = {};
  reset_output(true);
  // close interrupts an in-flight open and is queued after any current command.
  // Dropping SDK promise futures doesn't wait for their worker.
  command(Operation::close, player_.close());
}
void Session::create_audio() {
  if (!snapshot().media.has_audio) return;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    warning_ = std::string("音频设备不可用，将静音播放：") + SDL_GetError(); return;
  }
  SDL_AudioSpec desired{};
  desired.freq = 48000; desired.format = AUDIO_F32SYS; desired.channels = 2; desired.samples = 512;
  audio_ = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
  if (!audio_) warning_ = std::string("音频设备不可用，将静音播放：") + SDL_GetError();
}
void Session::upload(const sdk::media::VideoFrame& frame) {
  if (!texture_ || frame.width != width_ || frame.height != height_) {
    SDL_DestroyTexture(texture_);
    width_ = frame.width; height_ = frame.height;
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width_, height_);
    check_sdl(texture_ != nullptr);
  }
  check_sdl(SDL_UpdateTexture(texture_, nullptr, frame.rgba->data(), frame.stride) == 0);
}
bool Session::drained() const {
  return !busy() && snapshot().state == sdk::PlayerState::ended && (!audio_ || !SDL_GetQueuedAudioSize(audio_));
}
void Session::update() {
  try {
    if (operation_ == Operation::open && opening_.wait_for(0ms) == std::future_status::ready) {
      opening_.get(); operation_ = Operation::none;
      path_ = opening_path_; generation_ = snapshot().generation;
      create_audio();
      command(Operation::play, player_.play());
    } else if (operation_ != Operation::none && operation_ != Operation::open &&
               pending_.wait_for(0ms) == std::future_status::ready) {
      const auto completed = operation_;
      pending_.get(); operation_ = Operation::none;
      if (completed == Operation::play && audio_ && audio_started_) SDL_PauseAudioDevice(audio_, 0);
      auto after = std::move(after_); after_ = {};
      if (after) after();
    }
    if (busy() || closing_) return;
    auto state = snapshot();
    if (state.state == sdk::PlayerState::failed) {
      error_ = state.error; reset_output(true); return;
    }
    if (generation_ != state.generation) { reset_output(true); generation_ = state.generation; }
    // Device queue capped at 120 ms; gain applies to newly queued samples.
    while (!audio_ || SDL_GetQueuedAudioSize(audio_) < 48000 * 2 * sizeof(float) * 12 / 100) {
      auto frame = player_.poll_audio();
      if (!frame) break;
      if (audio_) {
        const float gain = muted ? 0 : std::clamp(volume, 0.0f, 1.0f);
        audio_buffer_.resize(frame->pcm->size());
        std::transform(frame->pcm->begin(), frame->pcm->end(), audio_buffer_.begin(), [gain](float value) { return value * gain; });
        check_sdl(SDL_QueueAudio(audio_, audio_buffer_.data(), static_cast<Uint32>(audio_buffer_.size() * sizeof(float))) == 0);
        if (!audio_started_) { SDL_PauseAudioDevice(audio_, 0); audio_started_ = true; }
      }
    }
    std::optional<sdk::media::VideoFrame> latest;
    while (auto frame = player_.poll_video()) latest = std::move(frame);
    if (latest) upload(*latest);
  } catch (const std::exception& exception) {
    error_ = exception.what(); operation_ = Operation::none; after_ = {};
    reset_output(true);
    // A UI/device failure must not leave decoding running behind an error screen.
    if (!closing_) command(Operation::close, player_.close());
  }
}
}
