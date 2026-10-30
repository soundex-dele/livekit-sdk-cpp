#pragma once
// Build-tree application/test helper, not an installed SDK API. SDK events are
// callbacks; these existing applications marshal them to their session loop.
#include "livekit/rtc/engine.hpp"
#include "rtc_result.hpp"
#include "rtc_blocking_bridge.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <type_traits>
#include <stdexcept>

namespace livekit::sdk::app_support {
class RtcEventMailbox {
  struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<rtc::RoomEvent> events;
    std::deque<rtc::engine::RtcEngine::GraphFailure> failures;
    bool overflow = false;
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
          state->overflow = true;
        } else {
          state->events.push_back(event);
        }
        state->changed.notify_all();
      }
    };
    if constexpr (std::is_same_v<Engine, BlockingRoom>) {
      require_result(engine.set_event_handler(event));
    } else {
      require_result(engine.set_callbacks({event, [weak](const auto& failure) {
                                             if (auto state = weak.lock()) {
                                               std::lock_guard lock(state->mutex);
                                               if (state->failures.size() >= 256) {
                                                 state->overflow = true;
                                               } else {
                                                 state->failures.push_back(failure);
                                               }
                                               state->changed.notify_all();
                                             }
                                           }}));
    }
  }

  std::optional<rtc::RoomEvent> take_event(std::chrono::milliseconds wait = {}) {
    std::unique_lock lock(state_->mutex);
    state_->changed.wait_for(lock, wait,
                             [&] { return state_->overflow || !state_->events.empty(); });
    if (state_->overflow) {
      throw std::runtime_error("Application RTC event mailbox exhausted");
    }
    if (state_->events.empty()) {
      return {};
    }
    auto event = std::move(state_->events.front());
    state_->events.pop_front();
    return event;
  }

  std::optional<rtc::engine::RtcEngine::GraphFailure> take_failure() {
    std::lock_guard lock(state_->mutex);
    if (state_->overflow) {
      throw std::runtime_error("Application RTC event mailbox exhausted");
    }
    if (state_->failures.empty()) {
      return {};
    }
    auto failure = std::move(state_->failures.front());
    state_->failures.pop_front();
    return failure;
  }
};
}  // namespace livekit::sdk::app_support
