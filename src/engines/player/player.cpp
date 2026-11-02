#include "livekit/player.hpp"
#include "livekit/media_core/clock.hpp"
#include "engines/player/playback_graph.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <limits>
#include <thread>
#include <variant>
#include <utility>

namespace livekit::sdk {
class Player::Impl {
 public:
  explicit Impl(PlayerOptions options) : options_(options) {
    if (options.io_timeout.count() <= 0 || !options.video_queue_frames ||
        !options.audio_queue_frames || !options.queue_bytes) {
      configuration_failure_ = make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                            "player: timeout and queue limits must be positive");
    }
    worker_ = std::thread([this] { run(); });
    delivery_ = std::thread([this] { deliver(); });
  }

  ~Impl() {
    {
      std::lock_guard lock(mutex_);
      quitting_ = true;
      cancelled_ = true;
      signal_locked();
    }
    worker_.join();
    delivery_.join();
  }

  void signal_locked(bool state = false) {
    state_dirty_ |= state;
    ++revision_;
    changed_.notify_all();
  }

  void fence_delivery() {
    std::unique_lock lock(mutex_);
    delivery_suspended_ = true;
    changed_.wait(lock, [&] { return !callback_running_; });
  }

  void resume_delivery() {
    std::lock_guard lock(mutex_);
    delivery_suspended_ = false;
    signal_locked(true);
  }

  template <class T, class F>
  AsyncResult<T> submit(F operation, bool interrupt = false, bool interrupt_control = true) {
    auto promise = std::make_shared<CompletionSource<T>>();
    auto result = promise->get_result();
    {
      std::lock_guard lock(mutex_);
      if (configuration_failure_ || quitting_ || commands_.size() >= 1024) {
        promise->set_failure(configuration_failure_
                                 ? configuration_failure_
                                 : make_failure(ErrorDomain::lifecycle,
                                                quitting_ ? ErrorCode::session_closed
                                                          : ErrorCode::resource_exhausted,
                                                "Player command admission rejected"));
        return result;
      }
      if (interrupt && (interrupt_control || !command_running_)) {
        cancelled_ = true;
      }
      commands_.push_back([this, promise, operation = std::move(operation)]() mutable {
        fence_delivery();
        auto completed = operation();
        if (!completed && completed.error().code != ErrorCode::invalid_argument &&
            completed.error().code != ErrorCode::invalid_state) {
          completed = Result<T>::failure(fail(completed.failure_record()));
        }
        resume_delivery();
        promise->set_result(std::move(completed));
      });
      signal_locked();
    }
    return result;
  }

  Failure fail(Failure cause) {
    std::lock_guard lock(mutex_);
    if (snapshot_.state == PlayerState::failed && snapshot_.failure) {
      return snapshot_.failure;
    }
    auto info = with_context(std::move(cause), {"player", {}, {}, snapshot_.generation});
    snapshot_.state = PlayerState::failed;
    snapshot_.error = info->message;
    snapshot_.failure = info;
    preview_pending_ = false;
    preview_.reset();
    audio_credits_ = 0;
    clock_.pause();
    if (outputs_) {
      outputs_->clear();
    }
    signal_locked(true);
    return info;
  }

  void clear_locked(int64_t position, PlayerState state) {
    eof_ = false;
    end_us_ = position;
    preview_pending_ = false;
    preview_dirty_ = false;
    preview_.reset();
    ++snapshot_.generation;
    if (outputs_) {
      outputs_->flush(snapshot_.generation);
    }
    snapshot_.state = state;
    snapshot_.error.clear();
    snapshot_.failure.reset();
    audio_credits_ = 0;
    signal_locked(true);
    clock_.reset(position, state == PlayerState::playing);
  }

  Result<PlayerMediaInfo> open_worker(const std::string& path) {
    auto closed = close_worker();
    if (!closed) {
      return Result<PlayerMediaInfo>::failure(closed.failure_record());
    }
    auto graph = std::make_unique<detail::PlaybackGraph>(cancelled_, options_);
    auto info = graph->open(path);
    if (!info) {
      return info;
    }
    graph_ = std::move(graph);
    std::lock_guard lock(mutex_);
    outputs_ = graph_->outputs();
    snapshot_.media = *info;
    clear_locked(0, PlayerState::ready);
    return info;
  }

  Result<void> close_worker() {
    graph_.reset();
    std::lock_guard lock(mutex_);
    snapshot_.media = {};
    clear_locked(0, PlayerState::closed);
    outputs_.reset();
    return Result<void>::success();
  }

  Result<void> require_open() {
    std::lock_guard lock(mutex_);
    if (!graph_ || snapshot_.state == PlayerState::failed) {
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                                "player: open a media file first"));
    }
    return Result<void>::success();
  }

  Result<void> seek_worker(int64_t position, bool stop = false) {
    auto opened = require_open();
    if (!opened) {
      return opened;
    }
    PlayerState next;
    {
      std::lock_guard lock(mutex_);
      if (position < 0 ||
          (snapshot_.media.duration_us > 0 && position > snapshot_.media.duration_us)) {
        return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                                  "player/seek: position outside media duration"));
      }
      next = stop                                      ? PlayerState::ready
             : snapshot_.state == PlayerState::playing ? PlayerState::playing
                                                       : PlayerState::paused;
      clock_.pause();
    }
    {
      auto controlled = graph_->pause();
      if (!controlled) {
        return controlled;
      }
    }
    {
      auto controlled = graph_->seek(position);
      if (!controlled) {
        return controlled;
      }
    }
    bool preview;
    {
      std::lock_guard lock(mutex_);
      clear_locked(position, next);
      preview = next == PlayerState::paused && snapshot_.media.has_video;
      preview_pending_ = preview_dirty_ = preview;
      preview_deadline_ = std::chrono::steady_clock::now() + options_.io_timeout;
    }
    if (next == PlayerState::playing || preview) {
      auto controlled = graph_->start();
      if (!controlled) {
        return controlled;
      }
    }
    return Result<void>::success();
  }

  Result<void> play_worker() {
    auto opened = require_open();
    if (!opened) {
      return opened;
    }
    bool ended, rewind_preview;
    int64_t position;
    {
      std::lock_guard lock(mutex_);
      ended = snapshot_.state == PlayerState::ended;
      rewind_preview = preview_dirty_;
      position = clock_.position_us();
    }
    if (ended) {
      auto controlled = seek_worker(0);
      if (!controlled) {
        return controlled;
      }
    }
    if (rewind_preview) {
      // Preview drains audio silently to reach a video frame even with tiny
      // queues. Re-seek before playback so no audio or video is lost.
      {
        auto controlled = graph_->pause();
        if (!controlled) {
          return controlled;
        }
      }
      {
        auto controlled = graph_->seek(position);
        if (!controlled) {
          return controlled;
        }
      }
      std::lock_guard lock(mutex_);
      clear_locked(position, PlayerState::paused);
    }
    {
      auto controlled = graph_->start();
      if (!controlled) {
        return controlled;
      }
    }
    std::lock_guard lock(mutex_);
    preview_pending_ = false;
    preview_dirty_ = false;
    preview_.reset();
    if (snapshot_.state != PlayerState::playing) {
      clock_.resume();
    }
    snapshot_.state = PlayerState::playing;
    return Result<void>::success();
  }

  Result<void> pause_worker() {
    auto opened = require_open();
    if (!opened) {
      return opened;
    }
    {
      auto controlled = graph_->pause();
      if (!controlled) {
        return controlled;
      }
    }
    std::lock_guard lock(mutex_);
    preview_pending_ = false;
    audio_credits_ = 0;
    if (snapshot_.state == PlayerState::playing) {
      clock_.pause();
      snapshot_.state = PlayerState::paused;
    }
    return Result<void>::success();
  }

  PlayerSnapshot snapshot_locked() const {
    auto result = snapshot_;
    result.position_us = clock_.position_us();
    if (result.media.duration_us > 0) {
      result.position_us = std::min(result.position_us, result.media.duration_us);
    }
    result.queued_audio = outputs_ ? outputs_->audio->size() : 0;
    result.queued_video = outputs_ ? outputs_->video->size() : 0;
    result.pending_commands = commands_.size();
    result.audio_demand = audio_credits_;
    return result;
  }

  PlayerSnapshot snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_locked();
  }

  struct CallbackSlot {
    PlayerCallbacks callbacks;
    bool running = false;
  };

  Result<void> set_callbacks(PlayerCallbacks callbacks, PlayerOutputOptions options) {
    if (options.audio_ahead.count() < 0 || options.audio_ahead > std::chrono::milliseconds(100)) {
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                                "player: audio lead must be between 0 and 100 ms"));
    }
    auto next = std::make_shared<CallbackSlot>();
    next->callbacks = std::move(callbacks);
    std::unique_lock lock(mutex_);
    auto previous = std::exchange(callbacks_, std::move(next));
    // Self-replacement may retire a handler while it is still running. Keep
    // fencing that invocation even when a later registration replaces its successor.
    auto active = in_flight_;
    output_options_ = options;
    audio_credits_ = 0;
    signal_locked(true);
    if (active && std::this_thread::get_id() != delivery_.get_id()) {
      changed_.wait(lock, [&] { return !active->running; });
    }
    lock.unlock();
    return Result<void>::success();
  }

  bool request_audio(uint64_t generation, size_t frames) {
    std::lock_guard lock(mutex_);
    if (!frames || generation != snapshot_.generation || snapshot_.state != PlayerState::playing ||
        output_options_.audio != PlayerAudioDelivery::on_demand || !callbacks_ ||
        !callbacks_->callbacks.audio || frames > options_.audio_queue_frames - audio_credits_) {
      return false;
    }
    audio_credits_ += frames;
    signal_locked();
    return true;
  }

  void deliver() {
    using Clock = std::chrono::steady_clock;
    using Payload = std::variant<PlayerSnapshot, media::VideoFrame, media::AudioFrame>;
    bool prefer_audio = false;
    for (;;) {
      std::unique_lock lock(mutex_);
      if (quitting_) {
        return;
      }
      const auto observed = revision_;
      std::shared_ptr<CallbackSlot> target = callbacks_;
      std::optional<Payload> payload;
      std::optional<Clock::time_point> deadline;
      const auto due_at = [&](std::optional<int64_t> timestamp, int64_t lead) {
        if (!timestamp) {
          return;
        }
        const auto delay = *timestamp - clock_.position_us() - lead;
        if (delay <= 0) {
          return;
        }
        const auto at = Clock::now() + std::chrono::microseconds(delay);
        if (!deadline || at < *deadline) {
          deadline = at;
        }
      };
      const auto consumed = [&] {
        ++consumed_;
        signal_locked();
      };
      if (!delivery_suspended_) {
        if (state_dirty_) {
          state_dirty_ = false;
          if (target && target->callbacks.state) {
            payload = snapshot_locked();
          }
        }
        if (!payload && snapshot_.state == PlayerState::paused && preview_) {
          if (target && target->callbacks.video) {
            payload = std::move(*preview_);
          }
          preview_.reset();
          consumed();
        }
        if (!payload && snapshot_.state == PlayerState::playing && outputs_) {
          const auto video = [&] {
            auto frame = outputs_->video->take_due(clock_.position_us(), snapshot_.generation);
            if (frame) {
              if (target && target->callbacks.video) {
                payload = std::move(*frame);
              }
              consumed();
            } else {
              due_at(outputs_->video->next_timestamp(), 0);
            }
          };
          const auto audio = [&] {
            const bool enabled = target && target->callbacks.audio;
            if (enabled && output_options_.audio == PlayerAudioDelivery::on_demand &&
                !audio_credits_) {
              return;
            }
            const auto lead = output_options_.audio_ahead.count();
            auto frame =
                outputs_->audio->take_due(clock_.position_us() + lead, snapshot_.generation);
            if (frame) {
              if (enabled) {
                if (output_options_.audio == PlayerAudioDelivery::on_demand) {
                  --audio_credits_;
                }
                payload = std::move(*frame);
              }
              consumed();
            } else {
              due_at(outputs_->audio->next_timestamp(), lead);
            }
          };
          if (prefer_audio) {
            audio();
            if (!payload) {
              video();
            }
          } else {
            video();
            if (!payload) {
              audio();
            }
          }
          prefer_audio = !prefer_audio;
        }
      }
      if (!payload) {
        const auto changed = [&] { return quitting_ || revision_ != observed; };
        if (deadline) {
          changed_.wait_until(lock, *deadline, changed);
        } else {
          changed_.wait(lock, changed);
        }
        lock.unlock();
        continue;
      }
      callback_running_ = target->running = true;
      in_flight_ = target;
      lock.unlock();
      auto delivered = std::visit(
          [&](const auto& value) -> Result<void> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, PlayerSnapshot>) {
              return target->callbacks.state(value);
            } else if constexpr (std::is_same_v<T, media::VideoFrame>) {
              return target->callbacks.video(value);
            } else {
              return target->callbacks.audio(value);
            }
          },
          *payload);
      if (!delivered) {
        // A failed media handler is disabled; state delivery can still report the
        // failure. A failed state handler is also disabled, preventing recursion.
        PlayerCallbacks retired;
        {
          std::lock_guard guard(mutex_);
          if (callbacks_ == target) {
            retired.audio = std::move(target->callbacks.audio);
            retired.video = std::move(target->callbacks.video);
            if (std::holds_alternative<PlayerSnapshot>(*payload)) {
              retired.state = std::move(target->callbacks.state);
            }
          }
        }
        fail(delivered.failure_record());
      }
      lock.lock();
      callback_running_ = target->running = false;
      in_flight_.reset();
      ++consumed_;
      signal_locked();
      // Captures may own application objects; retire them without the player lock.
      lock.unlock();
    }
  }

  void run() {
    for (;;) {
      std::function<void()> command;
      uint64_t consumed;
      {
        std::unique_lock lock(mutex_);
        if (quitting_) {
          break;
        }
        consumed = consumed_;
        if (!commands_.empty()) {
          command_running_ = true;
          command = std::move(commands_.front());
          commands_.pop_front();
          cancelled_ = false;
        } else if (snapshot_.state == PlayerState::playing || preview_pending_) {
          if (eof_) {
            if (outputs_->empty() && !callback_running_ && clock_.position_us() >= end_us_) {
              clock_.reset(end_us_);
              snapshot_.state = PlayerState::ended;
              signal_locked(true);
              lock.unlock();
              auto paused = graph_->pause();
              if (!paused) {
                fail(paused.failure_record());
              }
              continue;
            }
            const auto observed = revision_;
            const auto changed = [&] { return quitting_ || revision_ != observed; };
            if (outputs_->empty() && !callback_running_) {
              changed_.wait_until(
                  lock,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(std::max<int64_t>(
                                                         0, end_us_ - clock_.position_us())),
                  changed);
            } else {
              changed_.wait(lock, changed);
            }
            continue;
          }
        } else {
          changed_.wait(lock, [&] {
            return quitting_ || !commands_.empty() || snapshot_.state == PlayerState::playing ||
                   preview_pending_;
          });
          continue;
        }
      }
      if (command) {
        command();
        {
          std::lock_guard lock(mutex_);
          command_running_ = false;
        }
        continue;
      }
      {
        const auto step = graph_->step();
        if (!step) {
          bool superseded;
          {
            std::lock_guard lock(mutex_);
            superseded = cancelled_ && (!commands_.empty() || quitting_);
          }
          if (!superseded) {
            fail(step.failure_record());
          }
          continue;
        }
        std::unique_lock lock(mutex_);
        end_us_ = graph_->end_us();
        signal_locked();  // Source progress wakes delivery; no timed readiness scan.
        if (snapshot_.state == PlayerState::failed) {
          outputs_->clear();
          continue;
        }
        if (preview_pending_) {
          constexpr auto all = std::numeric_limits<int64_t>::max();
          preview_ = outputs_->video->take_due(all, snapshot_.generation);
          while (outputs_->audio->take_due(all, snapshot_.generation)) {
          }
          if (preview_ || *step == detail::PlaybackGraph::Step::end) {
            preview_pending_ = false;
            outputs_->clear();
            lock.unlock();
            auto paused = graph_->pause();
            if (!paused) {
              fail(paused.failure_record());
            }
            continue;
          }
          if (std::chrono::steady_clock::now() >= preview_deadline_) {
            lock.unlock();
            fail(make_failure(ErrorDomain::media, ErrorCode::timeout,
                              "player/preview: timed out decoding the target frame"));
            continue;
          }
        }
        if (*step == detail::PlaybackGraph::Step::end) {
          eof_ = true;
        }
        if (*step == detail::PlaybackGraph::Step::backpressured && !preview_pending_) {
          changed_.wait(lock, [&] {
            return quitting_ || !commands_.empty() || consumed_ != consumed ||
                   snapshot_.state == PlayerState::failed || cancelled_;
          });
        }
      }
    }
    graph_.reset();
  }

  PlayerOptions options_;
  Failure configuration_failure_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<std::function<void()>> commands_;
  std::atomic<bool> cancelled_{false};
  bool quitting_ = false, eof_ = false, command_running_ = false;
  bool delivery_suspended_ = false, callback_running_ = false, state_dirty_ = false;
  uint64_t revision_ = 0, consumed_ = 0;
  size_t audio_credits_ = 0;
  std::shared_ptr<CallbackSlot> callbacks_, in_flight_;
  PlayerOutputOptions output_options_;
  bool preview_pending_ = false, preview_dirty_ = false;
  std::optional<media::VideoFrame> preview_;
  std::chrono::steady_clock::time_point preview_deadline_;
  int64_t end_us_ = 0;
  PlayerSnapshot snapshot_;
  media::MediaClock clock_;
  std::shared_ptr<detail::PlaybackOutputs> outputs_;
  std::unique_ptr<detail::PlaybackGraph> graph_;
  std::thread worker_, delivery_;
};

Player::Player(PlayerOptions options) : impl_(std::make_unique<Impl>(options)) {}

Player::~Player() = default;

AsyncResult<PlayerMediaInfo> Player::open(std::string path) {
  return impl_->submit<PlayerMediaInfo>(
      [this, path = std::move(path)] { return impl_->open_worker(path); }, true);
}

AsyncResult<void> Player::play() {
  return impl_->submit<void>([this] { return impl_->play_worker(); });
}

AsyncResult<void> Player::pause() {
  return impl_->submit<void>([this] { return impl_->pause_worker(); });
}

AsyncResult<void> Player::seek(std::chrono::microseconds position) {
  return impl_->submit<void>([this, position] { return impl_->seek_worker(position.count()); },
                             true, false);
}

AsyncResult<void> Player::stop() {
  return impl_->submit<void>([this] { return impl_->seek_worker(0, true); }, true, false);
}

AsyncResult<void> Player::close() {
  return impl_->submit<void>([this] { return impl_->close_worker(); }, true);
}

void Player::cancel_pending_io() {
  std::lock_guard lock(impl_->mutex_);
  impl_->cancelled_ = true;
  impl_->signal_locked();
}

PlayerSnapshot Player::snapshot() const {
  return impl_->snapshot();
}

Result<void> Player::set_callbacks(PlayerCallbacks callbacks, PlayerOutputOptions options) {
  return impl_->set_callbacks(std::move(callbacks), options);
}

bool Player::request_audio(uint64_t generation, size_t frames) {
  return impl_->request_audio(generation, frames);
}
}  // namespace livekit::sdk
