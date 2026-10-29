#pragma once
#include "livekit/media_core/frame.hpp"
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace livekit::sdk {
enum class PlayerState { closed, ready, playing, paused, ended, failed };

struct PlayerMediaInfo {
  int64_t duration_us = 0;  // 0 when unavailable
  bool has_video = false, has_audio = false;
  int width = 0, height = 0;
  int audio_sample_rate = 48000, audio_channels = 2;
};

struct PlayerOptions {
  size_t video_queue_frames = 8, audio_queue_frames = 64;
  size_t queue_bytes = 128 * 1024 * 1024;  // per branch
  std::chrono::milliseconds io_timeout{10000};
};

struct PlayerSnapshot {
  PlayerState state = PlayerState::closed;
  PlayerMediaInfo media;
  int64_t position_us = 0;
  uint64_t generation = 0;
  size_t queued_video = 0, queued_audio = 0;
  std::string error;
};

class PlayerError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Independent of RTC, FFmpeg and GraphFlow headers. One serialized worker owns
// the private graph; control completion is reported by futures, never callbacks.
// Public methods are thread safe. Destruction requires callers to stop using
// the object, interrupts I/O and joins the worker. No callbacks outlive close.
class Player {
 public:
  explicit Player(PlayerOptions options = {});
  ~Player();
  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;
  std::future<PlayerMediaInfo> open(std::string utf8_local_path);
  std::future<void> play();
  std::future<void> pause();
  // Paused seek asynchronously prepares one target video frame for poll_video;
  // the clock remains frozen and poll_audio stays empty until play().
  std::future<void> seek(std::chrono::microseconds position);
  std::future<void> stop();   // rewind to zero, retain the source, transition to ready
  std::future<void> close();  // idempotent; may open again after completion
  void cancel_pending_io();   // interrupted open/read/seek reports an error
  PlayerSnapshot snapshot() const;
  // Pull output on the application/device thread; no user code runs on the
  // control worker. Poll both existing streams until ended. Queues use backpressure.
  // Video is due at the media clock; audio may be queued up to 100 ms ahead.
  // Pause/seek/stop/close require the consumer to pause/flush its own device queues.
  // Discard externally retained frames whose generation differs from snapshot().
  std::optional<media::VideoFrame> poll_video();
  std::optional<media::AudioFrame> poll_audio();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk
