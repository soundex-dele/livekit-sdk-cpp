#pragma once
#include "livekit/media_core/frame.hpp"
#include "livekit/async_result.hpp"
#include "livekit/error.hpp"
#include <functional>
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
  std::shared_ptr<const FailureInfo> failure;
  size_t pending_commands = 0, audio_demand = 0;
};

class PlayerError : public SDKError {
 public:
  explicit PlayerError(std::string message)
      : SDKError(ErrorDomain::media, ErrorCode::processing_failed, std::move(message)) {}

  explicit PlayerError(std::shared_ptr<const FailureInfo> failure) : SDKError(std::move(failure)) {}
};

enum class PlayerAudioDelivery { clocked, on_demand };

struct PlayerOutputOptions {
  PlayerAudioDelivery audio = PlayerAudioDelivery::clocked;
  std::chrono::microseconds audio_ahead{100000};  // 0..100 ms on the file clock.
};

struct PlayerCallbacks {
  std::function<void(const media::VideoFrame&)> video;
  std::function<void(const media::AudioFrame&)> audio;
  std::function<void(const PlayerSnapshot&)> state;
};

// Independent of RTC, FFmpeg and GraphFlow headers. The source worker owns the
// graph and performs bounded decoding. A separate serial delivery worker invokes
// application callbacks. No application polling is needed to progress playback.
// Public methods are thread safe; serialize callback registration. Destruction
// requires callers to stop using the object and fences both workers.
class Player {
 public:
  explicit Player(PlayerOptions options = {});
  ~Player();
  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;
  AsyncResult<PlayerMediaInfo> open(std::string utf8_local_path);
  AsyncResult<void> play();
  AsyncResult<void> pause();
  // Paused seek asynchronously delivers one target video frame. The clock stays
  // frozen; audio callbacks resume only after play().
  AsyncResult<void> seek(std::chrono::microseconds position);
  AsyncResult<void> stop();   // rewind to zero, retain the source, transition to ready
  AsyncResult<void> close();  // idempotent; may open again after completion
  void cancel_pending_io();   // interrupted open/read/seek reports an error
  PlayerSnapshot snapshot() const;
  // Video is delivered when due; audio follows the clock or explicit demand.
  // Missing callbacks discard that stream. Frames own immutable shared storage.
  // Replacing/clearing fences old callbacks; self-removal is supported. Callbacks
  // must return promptly and must not wait for lifecycle results or destroy this
  // player: lifecycle completion fences delivery. Exceptions fail the player and
  // retain the same FailureInfo in its state callback and snapshot.
  void set_callbacks(PlayerCallbacks, PlayerOutputOptions = {});
  // On-demand audio grants bounded credits, one per complete decoded audio frame.
  // Accepted only while playing, for the current generation, with an audio handler.
  // Returns false for a stale generation, disabled output, or exhausted credit
  // budget. Pause/seek/stop/close reset credits. No device work runs in this call.
  bool request_audio(uint64_t generation, size_t frames = 1);
  // Pause fences callbacks; seek/stop/close also invalidate retained frames.
  // The application must pause/flush its device buffers on these transitions.

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk
