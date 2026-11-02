#pragma once
// Build-tree application/test helper, not an installed SDK API. SDK events are
// callbacks; these existing applications marshal them to their session loop.
#include "livekit/rtc/engine.hpp"
#include "livekit/result.hpp"
#include "rtc_blocking_bridge.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <type_traits>

namespace livekit::sdk::app_support {
class RtcEventMailbox {
  struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<rtc::RoomEvent> events;
    std::deque<rtc::engine::GraphFailure> failures;
    Failure failure;
  };

  std::shared_ptr<State> state_ = std::make_shared<State>();

 public:
  RtcEventMailbox() = default;

  template <class Engine>
  explicit RtcEventMailbox(Engine& engine) {
    attach(engine);
  }

  template <class Engine>
  void attach(Engine& engine) {
    auto weak = std::weak_ptr(state_);
    const auto event = [weak](const rtc::RoomEvent& event) {
      if (auto state = weak.lock()) {
        std::lock_guard lock(state->mutex);
        if (state->events.size() >= 4096) {
          if (!state->failure) {
            state->failure = make_failure(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                                          "Application RTC event mailbox exhausted");
          }
        } else {
          state->events.push_back(event);
        }
        state->changed.notify_all();
      }
    };
    if constexpr (std::is_same_v<Engine, BlockingRoom>) {
      record(engine.set_event_handler(event));
    } else {
      record(engine.set_callbacks({event, [weak](const auto& failure) {
                                     if (auto state = weak.lock()) {
                                       std::lock_guard lock(state->mutex);
                                       if (state->failures.size() >= 256) {
                                         if (!state->failure) {
                                           state->failure = make_failure(
                                               ErrorDomain::lifecycle,
                                               ErrorCode::resource_exhausted,
                                               "Application RTC event mailbox exhausted");
                                         }
                                       } else {
                                         state->failures.push_back(failure);
                                       }
                                       state->changed.notify_all();
                                     }
                                   }}));
    }
  }

  Result<std::optional<rtc::RoomEvent>> take_event(std::chrono::milliseconds wait = {}) {
    std::unique_lock lock(state_->mutex);
    state_->changed.wait_for(lock, wait,
                             [&] { return state_->failure || !state_->events.empty(); });
    if (state_->failure) {
      return Result<std::optional<rtc::RoomEvent>>::failure(state_->failure);
    }
    if (state_->events.empty()) {
      return Result<std::optional<rtc::RoomEvent>>::success({});
    }
    auto event = std::move(state_->events.front());
    state_->events.pop_front();
    return Result<std::optional<rtc::RoomEvent>>::success(std::move(event));
  }

  Result<std::optional<rtc::engine::GraphFailure>> take_failure() {
    std::lock_guard lock(state_->mutex);
    if (state_->failure) {
      return Result<std::optional<rtc::engine::GraphFailure>>::failure(state_->failure);
    }
    if (state_->failures.empty()) {
      return Result<std::optional<rtc::engine::GraphFailure>>::success({});
    }
    auto failure = std::move(state_->failures.front());
    state_->failures.pop_front();
    return Result<std::optional<rtc::engine::GraphFailure>>::success(std::move(failure));
  }

  Result<void> status() const {
    std::lock_guard lock(state_->mutex);
    return state_->failure ? Result<void>::failure(state_->failure) : Result<void>::success();
  }

 private:
  void record(ErrorCode code) {
    if (code != ErrorCode::ok) {
      std::lock_guard lock(state_->mutex);
      state_->failure = make_failure(ErrorDomain::lifecycle, code, "Cannot attach RTC mailbox");
    }
  }
};
}  // namespace livekit::sdk::app_support
