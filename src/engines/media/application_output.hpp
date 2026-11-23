#pragma once
#include "livekit/compat/thread.hpp"
#include "livekit/rtc/media_config.hpp"
#include "livekit/rtc/frame_view.hpp"
#include "livekit/rtc/video_nodes.hpp"
#include "livekit/rtc/audio_nodes.hpp"
#include "media_graph/callback_context.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>

namespace livekit::sdk::rtc::engine {
// One bounded dispatcher per application branch. Payload ownership is shared;
// invoking a slow application never occupies a codec/capture worker.
class ApplicationOutput final : public livekit::sdk::rtc::TrackNode {
 public:
  explicit ApplicationOutput(MediaOutput output, bool internal_inline = false)
      : TrackNode("application.output"), output_(std::move(output)), inline_(internal_inline) {
    if (!output_.queue.max_frames || !output_.queue.max_bytes ||
        output_.queue.max_age.count() <= 0) {
      failure_ = make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                              "Invalid application output queue");
    }
  }

  ~ApplicationOutput() override {
    quiesce();
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
    report_failure(std::move(failure));
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

  void bind(graphflow::Binder& ports) override {
    ports.input(livekit::sdk::rtc::TrackPort<nodes::VideoFrameEvent>::in,
                [this](const auto& event) {
                  if (!event.pixels) {
                    fail(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                      "Missing I420 video payload"));
                    return;
                  }
                  push(event, track_cost(event).bytes);
                });
    ports.input(livekit::sdk::rtc::TrackPort<nodes::AudioFrameEvent>::in,
                [this](const auto& event) {
                  if (!event.pcm) {
                    fail(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                      "Missing PCM audio payload"));
                    return;
                  }
                  push(event, track_cost(event).bytes);
                });
  }

  livekit::sdk::Result<void> prepare() override {
    if (auto valid = status(); !valid) {
      report_failure(valid.failure_record());
      return start_error();
    }
    return livekit::sdk::Result<void>::success();
  }

  livekit::sdk::Result<void> activate_graph() override {
    active_ = true;
    if (!inline_ && (output_.audio || output_.video || output_.frame)) {
      worker_ = livekit::compat::jthread([this](livekit::compat::stop_token stop) {
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
    return livekit::sdk::Result<void>::success();
  }

  void stop_graph() override {
    quiesce();
  }

  MediaOutput output_;
  const bool inline_;
  mutable std::mutex mutex_;
  livekit::compat::condition_variable_any changed_;
  std::deque<Pending> queue_;
  size_t queued_bytes_ = 0;
  bool active_ = false;
  Failure failure_;
  livekit::compat::jthread worker_;
};
}  // namespace livekit::sdk::rtc::engine
