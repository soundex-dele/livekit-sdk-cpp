#pragma once
#include "media_graph/failure.hpp"
#include "livekit/rtc/media_config.hpp"
#include "livekit/rtc/frame_view.hpp"
#include "livekit/rtc/video_nodes.hpp"
#include "livekit/rtc/audio_nodes.hpp"
#include "backends/rtc/control_context.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>

namespace livekit::sdk::rtc::engine {
// One bounded dispatcher per application branch. Payload ownership is shared;
// invoking a slow application never occupies a codec/capture worker.
class ApplicationOutput final : public graphflow::core::GraphNode {
 public:
  explicit ApplicationOutput(MediaOutput output, bool internal_inline = false)
      : GraphNode("application.output", nullptr),
        output_(std::move(output)),
        inline_(internal_inline) {
    if (!output_.queue.max_frames || !output_.queue.max_bytes ||
        output_.queue.max_age.count() <= 0) {
      failure_ = make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                              "Invalid application output queue");
    }
  }

  ~ApplicationOutput() override {
    quiesce();
    release();
  }

  void quiesce() {
    {
      std::lock_guard lock(mutex_);
      active_ = false;
      queue_.clear();
      queued_bytes_ = 0;
    }
    worker_.request_stop();
    changed_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  Result<void> status() const {
    std::lock_guard lock(mutex_);
    if (failure_) {
      return Result<void>::failure(failure_);
    }
    return Result<void>::success();
  }

  std::atomic<uint64_t> frames{0}, bytes{0}, dropped{0};

 private:
  using Sample = std::variant<nodes::VideoFrameEvent, nodes::AudioFrameEvent>;

  struct Pending {
    Sample sample;
    size_t bytes;
    SteadyClock::time_point admitted;
  };

  Result<void> deliver(const Sample& sample) {
    detail::CallbackScope callback_scope;
    if (auto video = std::get_if<nodes::VideoFrameEvent>(&sample)) {
      if (output_.video) {
        auto result = output_.video({video->pixels, video->timestamp, video->epoch, video->captured,
                                     video->presentation_time});
        if (!result) {
          return result;
        }
      }
      if (output_.frame) {
        auto result = output_.frame(frame_view(video->pixels, *video));
        if (!result) {
          return result;
        }
      }
    } else {
      const auto& audio = std::get<nodes::AudioFrameEvent>(sample);
      if (output_.audio) {
        auto result = output_.audio({audio.pcm, audio.timestamp, audio.epoch, audio.captured,
                                     audio.discontinuity, audio.presentation_time});
        if (!result) {
          return result;
        }
      }
      if (output_.frame) {
        auto result = output_.frame(frame_view(audio.pcm, audio));
        if (!result) {
          return result;
        }
      }
    }
    return Result<void>::success();
  }

  void fail(Failure failure) {
    {
      std::lock_guard lock(mutex_);
      if (failure_) {
        return;
      }
      failure_ = failure;
      active_ = false;
      queue_.clear();
      queued_bytes_ = 0;
    }
    changed_.notify_all();
    reportError(livekit::sdk::detail::graph_error(std::move(failure)));
  }

  void push(Sample sample, size_t size) {
    ++frames;
    bytes += size;
    if (!output_.video && !output_.audio && !output_.frame) {
      return;
    }
    if (inline_) {
      if (auto result = deliver(sample); !result) {
        fail(result.failure_record());
      }
      return;
    }
    std::lock_guard lock(mutex_);
    if (!active_ || failure_ || size > output_.queue.max_bytes) {
      ++dropped;
      return;
    }
    while (!queue_.empty() && (queue_.size() >= output_.queue.max_frames ||
                               queued_bytes_ > output_.queue.max_bytes - size)) {
      queued_bytes_ -= queue_.front().bytes;
      queue_.pop_front();
      ++dropped;
    }
    queued_bytes_ += size;
    queue_.push_back({std::move(sample), size, SteadyClock::now()});
    changed_.notify_one();
  }

  bool doInit() override {
    if (auto valid = status(); !valid) {
      reportError(livekit::sdk::detail::graph_error(valid.failure_record()));
      return false;
    }
    subscribe<nodes::VideoFrameEvent>([this](const auto& event) {
      push(event, event.pixels->y.size() + event.pixels->u.size() + event.pixels->v.size());
    });
    subscribe<nodes::AudioFrameEvent>([this](const auto& event) {
      push(event, event.pcm->samples.size() * sizeof(int16_t) +
                      event.pcm->float_samples.size() * sizeof(float));
    });
    return true;
  }

  bool doStart() override {
    active_ = true;
    if (!inline_ && (output_.audio || output_.video || output_.frame)) {
      worker_ = std::jthread([this](std::stop_token stop) {
          while (!stop.stop_requested()) {
            std::optional<Pending> pending;
            {
              std::unique_lock lock(mutex_);
              changed_.wait(lock, stop, [&] { return !active_ || !queue_.empty(); });
              if (!active_ || stop.stop_requested()) {
                break;
              }
              pending = std::move(queue_.front());
              queue_.pop_front();
              queued_bytes_ -= pending->bytes;
            }
            if (SteadyClock::now() - pending->admitted > output_.queue.max_age) {
              ++dropped;
              continue;
            }
            const auto presentation = std::visit(
                [](const auto& sample) { return sample.presentation_time; }, pending->sample);
            if (presentation) {
              std::unique_lock lock(mutex_);
              changed_.wait_until(lock, stop, *presentation, [&] { return !active_; });
              if (!active_ || stop.stop_requested()) {
                break;
              }
              if (SteadyClock::now() - *presentation > output_.queue.max_age) {
                ++dropped;
                continue;
              }
            }
            if (auto result = deliver(pending->sample); !result) {
              fail(result.failure_record());
              break;
            }
          }

      });
    }
    return true;
  }

  void doStop() override {
    quiesce();
  }

  MediaOutput output_;
  const bool inline_;
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::deque<Pending> queue_;
  size_t queued_bytes_ = 0;
  bool active_ = false;
  Failure failure_;
  std::jthread worker_;
};
}  // namespace livekit::sdk::rtc::engine
