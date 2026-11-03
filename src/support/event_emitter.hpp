#pragma once
#include "livekit/json.hpp"
#include <functional>
#include <deque>
#include "livekit/result.hpp"
#include <mutex>

namespace lk {
// Producers never execute application code on WebRTC threads or under peer locks.
class EventEmitter {
  using Handler = std::function<void(const livekit::sdk::Json&)>;

  struct State {
    std::mutex mutex;
    std::deque<livekit::sdk::Json> pending;
    Handler handler;
    livekit::sdk::Failure failure;
  };

  std::shared_ptr<State> state_;

 public:
  explicit EventEmitter(Handler handler = {}) : state_(std::make_shared<State>()) {
    state_->handler = std::move(handler);
  }

  void operator()(const std::string& name,
                  livekit::sdk::Json fields = livekit::sdk::Json::object()) const {
    if (!state_->handler)
      return;
    fields["event"] = name;
    std::lock_guard lock(state_->mutex);
    if (state_->pending.size() >= 4096) {
      if (!state_->failure)
        state_->failure = livekit::sdk::make_failure(livekit::sdk::ErrorDomain::lifecycle,
                                                     livekit::sdk::ErrorCode::resource_exhausted,
                                                     "Room event queue overflow");
      return;
    }
    state_->pending.push_back(std::move(fields));
  }

  // Only the session thread drains. Handlers must not throw.
  void dispatch() const noexcept {
    std::deque<livekit::sdk::Json> events;
    {
      std::lock_guard lock(state_->mutex);
      events.swap(state_->pending);
    }
    for (const auto& value : events) {
      state_->handler(value);
    }
  }

  livekit::sdk::Result<void> status() const {
    std::lock_guard lock(state_->mutex);
    return state_->failure ? livekit::sdk::Result<void>::failure(state_->failure)
                           : livekit::sdk::Result<void>::success();
  }
};
}  // namespace lk
