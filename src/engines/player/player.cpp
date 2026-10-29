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

namespace livekit::sdk {
class Player::Impl {
 public:
  explicit Impl(PlayerOptions options) : options_(options) {
    if (options.io_timeout.count() <= 0)
      throw std::invalid_argument("player: timeout must be positive");
    if (!options.video_queue_frames || !options.audio_queue_frames || !options.queue_bytes)
      throw std::invalid_argument("player: queue limits must be positive");
    worker_ = std::thread([this] { run(); });
  }

  ~Impl() {
    {
      std::lock_guard lock(mutex_);
      quitting_ = true;
      cancelled_ = true;
    }
    changed_.notify_all();
    worker_.join();
  }

  template <class T, class F>
  std::future<T> submit(F operation, bool interrupt = false) {
    auto promise = std::make_shared<std::promise<T>>();
    auto future = promise->get_future();
    {
      std::lock_guard lock(mutex_);
      if (interrupt)
        cancelled_ = true;
      commands_.push_back([this, promise, operation = std::move(operation)]() mutable {
        try {
          if constexpr (std::is_void_v<T>) {
            operation();
            promise->set_value();
          } else
            promise->set_value(operation());
        } catch (const std::logic_error&) {
          // Invalid caller requests leave the existing session intact.
          promise->set_exception(std::current_exception());
        } catch (const std::exception& error) {
          fail(error.what());
          promise->set_exception(std::current_exception());
        } catch (...) {
          fail("player: unknown worker failure");
          promise->set_exception(std::current_exception());
        }
      });
    }
    changed_.notify_all();
    return future;
  }

  void fail(const std::string& error) {
    std::lock_guard lock(mutex_);
    snapshot_.state = PlayerState::failed;
    snapshot_.error = error;
    preview_pending_ = false;
    preview_.reset();
    clock_.pause();
    if (outputs_)
      outputs_->clear();
  }

  void clear_locked(int64_t position, PlayerState state) {
    eof_ = false;
    end_us_ = position;
    preview_pending_ = false;
    preview_dirty_ = false;
    preview_.reset();
    ++snapshot_.generation;
    if (outputs_)
      outputs_->flush(snapshot_.generation);
    snapshot_.state = state;
    snapshot_.error.clear();
    clock_.reset(position, state == PlayerState::playing);
  }

  PlayerMediaInfo open_worker(const std::string& path) {
    close_worker();
    auto graph = std::make_unique<detail::PlaybackGraph>(cancelled_, options_);
    auto info = graph->open(path);
    graph_ = std::move(graph);
    std::lock_guard lock(mutex_);
    outputs_ = graph_->outputs();
    snapshot_.media = info;
    clear_locked(0, PlayerState::ready);
    return info;
  }

  void close_worker() {
    graph_.reset();
    std::lock_guard lock(mutex_);
    snapshot_.media = {};
    clear_locked(0, PlayerState::closed);
    outputs_.reset();
  }

  void require_open() {
    std::lock_guard lock(mutex_);
    if (!graph_ || snapshot_.state == PlayerState::failed)
      throw std::logic_error("player: open a media file first");
  }

  void seek_worker(int64_t position, bool stop = false) {
    require_open();
    PlayerState next;
    {
      std::lock_guard lock(mutex_);
      if (position < 0 ||
          (snapshot_.media.duration_us > 0 && position > snapshot_.media.duration_us))
        throw std::invalid_argument("player/seek: position outside media duration");
      next = stop                                      ? PlayerState::ready
             : snapshot_.state == PlayerState::playing ? PlayerState::playing
                                                       : PlayerState::paused;
      clock_.pause();
    }
    graph_->pause();
    graph_->seek(position);
    bool preview;
    {
      std::lock_guard lock(mutex_);
      clear_locked(position, next);
      preview = next == PlayerState::paused && snapshot_.media.has_video;
      preview_pending_ = preview_dirty_ = preview;
      preview_deadline_ = std::chrono::steady_clock::now() + options_.io_timeout;
    }
    if (next == PlayerState::playing || preview)
      graph_->start();
  }

  void play_worker() {
    require_open();
    bool ended, rewind_preview;
    int64_t position;
    {
      std::lock_guard lock(mutex_);
      ended = snapshot_.state == PlayerState::ended;
      rewind_preview = preview_dirty_;
      position = clock_.position_us();
    }
    if (ended)
      seek_worker(0);
    if (rewind_preview) {
      // Preview drains audio silently to reach a video frame even with tiny
      // queues. Re-seek before playback so no audio or video is lost.
      graph_->pause();
      graph_->seek(position);
      std::lock_guard lock(mutex_);
      clear_locked(position, PlayerState::paused);
    }
    graph_->start();
    std::lock_guard lock(mutex_);
    preview_pending_ = false;
    preview_dirty_ = false;
    preview_.reset();
    if (snapshot_.state != PlayerState::playing)
      clock_.resume();
    snapshot_.state = PlayerState::playing;
  }

  void pause_worker() {
    require_open();
    graph_->pause();
    std::lock_guard lock(mutex_);
    preview_pending_ = false;
    if (snapshot_.state == PlayerState::playing) {
      clock_.pause();
      snapshot_.state = PlayerState::paused;
    }
  }

  PlayerSnapshot snapshot() const {
    std::lock_guard lock(mutex_);
    auto result = snapshot_;
    result.position_us = clock_.position_us();
    if (result.media.duration_us > 0)
      result.position_us = std::min(result.position_us, result.media.duration_us);
    result.queued_audio = outputs_ ? outputs_->audio->size() : 0;
    result.queued_video = outputs_ ? outputs_->video->size() : 0;
    return result;
  }

  template <class T>
  std::optional<T> poll(int64_t ahead) {
    std::lock_guard lock(mutex_);
    if constexpr (std::is_same_v<T, media::VideoFrame>) {
      if (snapshot_.state == PlayerState::paused && preview_) {
        auto result = std::move(preview_);
        preview_.reset();
        return result;
      }
    }
    if (snapshot_.state != PlayerState::playing || !outputs_)
      return {};
    const auto time = clock_.position_us() + ahead;
    std::optional<T> frame;
    if constexpr (std::is_same_v<T, media::VideoFrame>)
      frame = outputs_->video->poll(time, snapshot_.generation);
    else
      frame = outputs_->audio->poll(time, snapshot_.generation);
    if (frame)
      changed_.notify_all();
    return frame;
  }

  void run() {
    for (;;) {
      std::function<void()> command;
      {
        std::unique_lock lock(mutex_);
        if (quitting_)
          break;
        if (!commands_.empty()) {
          command = std::move(commands_.front());
          commands_.pop_front();
          cancelled_ = false;
        } else if (snapshot_.state == PlayerState::playing || preview_pending_) {
          if (eof_) {
            if (outputs_->empty() && clock_.position_us() >= end_us_) {
              clock_.reset(end_us_);
              snapshot_.state = PlayerState::ended;
              lock.unlock();
              graph_->pause();
              continue;
            }
            changed_.wait_for(lock, std::chrono::milliseconds(5));
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
        continue;
      }
      try {
        const auto step = graph_->step();
        std::unique_lock lock(mutex_);
        end_us_ = graph_->end_us();
        if (preview_pending_) {
          constexpr auto all = std::numeric_limits<int64_t>::max();
          preview_ = outputs_->video->poll(all, snapshot_.generation);
          while (outputs_->audio->poll(all, snapshot_.generation)) {
          }
          if (preview_ || step == detail::PlaybackGraph::Step::end) {
            preview_pending_ = false;
            outputs_->clear();
            lock.unlock();
            graph_->pause();
            continue;
          }
          if (std::chrono::steady_clock::now() >= preview_deadline_)
            throw PlayerError("player/preview: timed out decoding the target frame");
        }
        if (step == detail::PlaybackGraph::Step::end)
          eof_ = true;
        if (step == detail::PlaybackGraph::Step::backpressured && commands_.empty() && !quitting_)
          changed_.wait_for(lock, std::chrono::milliseconds(5));
      } catch (const std::exception& error) {
        // A queued seek/stop/close supersedes an interrupted read. Its command
        // will flush the pipeline before subsequent output can be returned.
        bool superseded;
        {
          std::lock_guard lock(mutex_);
          superseded = cancelled_ && (!commands_.empty() || quitting_);
        }
        if (!superseded)
          fail(error.what());
      } catch (...) {
        fail("player/decode: unknown error");
      }
    }
    graph_.reset();
  }

  PlayerOptions options_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<std::function<void()>> commands_;
  std::atomic<bool> cancelled_{false};
  bool quitting_ = false, eof_ = false;
  bool preview_pending_ = false, preview_dirty_ = false;
  std::optional<media::VideoFrame> preview_;
  std::chrono::steady_clock::time_point preview_deadline_;
  int64_t end_us_ = 0;
  PlayerSnapshot snapshot_;
  media::MediaClock clock_;
  std::shared_ptr<detail::PlaybackOutputs> outputs_;
  std::unique_ptr<detail::PlaybackGraph> graph_;
  std::thread worker_;
};

Player::Player(PlayerOptions options) : impl_(std::make_unique<Impl>(options)) {}

Player::~Player() = default;

std::future<PlayerMediaInfo> Player::open(std::string path) {
  return impl_->submit<PlayerMediaInfo>(
      [this, path = std::move(path)] { return impl_->open_worker(path); }, true);
}

std::future<void> Player::play() {
  return impl_->submit<void>([this] { impl_->play_worker(); });
}

std::future<void> Player::pause() {
  return impl_->submit<void>([this] { impl_->pause_worker(); });
}

std::future<void> Player::seek(std::chrono::microseconds position) {
  return impl_->submit<void>([this, position] { impl_->seek_worker(position.count()); });
}

std::future<void> Player::stop() {
  return impl_->submit<void>([this] { impl_->seek_worker(0, true); });
}

std::future<void> Player::close() {
  return impl_->submit<void>([this] { impl_->close_worker(); }, true);
}

void Player::cancel_pending_io() {
  impl_->cancelled_ = true;
}

PlayerSnapshot Player::snapshot() const {
  return impl_->snapshot();
}

std::optional<media::VideoFrame> Player::poll_video() {
  return impl_->poll<media::VideoFrame>(0);
}

std::optional<media::AudioFrame> Player::poll_audio() {
  return impl_->poll<media::AudioFrame>(100000);
}
}  // namespace livekit::sdk
