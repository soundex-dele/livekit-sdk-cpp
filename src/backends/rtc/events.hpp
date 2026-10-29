#pragma once
#include "livekit/json.hpp"
#include <functional>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>

namespace lk {
// Producers never execute application code on WebRTC threads or under peer locks.
class EventEmitter {
  using Handler = std::function<void(const livekit::sdk::Json&)>;
  struct State {
    std::mutex mutex;
    std::deque<livekit::sdk::Json> pending;
    Handler handler;
    std::exception_ptr failure;
  };
  std::shared_ptr<State> state_;
 public:
  explicit EventEmitter(Handler handler = {}) : state_(std::make_shared<State>()) {
    state_->handler = std::move(handler);
  }
  void operator()(const std::string& name, livekit::sdk::Json fields = livekit::sdk::Json::object()) const {
    if (!state_->handler) return;
    fields["event"] = name;
    std::lock_guard lock(state_->mutex);
    if (state_->pending.size() >= 4096) {
      if (!state_->failure) state_->failure = std::make_exception_ptr(std::runtime_error("Room event queue overflow"));
      return;
    }
    state_->pending.push_back(std::move(fields));
  }
  // Only the session thread drains; callback failures never escape a destructor.
  void dispatch() const noexcept {
    std::deque<livekit::sdk::Json> events;
    { std::lock_guard lock(state_->mutex); events.swap(state_->pending); }
    for (const auto& value : events) {
      try { state_->handler(value); }
      catch (...) { std::lock_guard lock(state_->mutex); if (!state_->failure) state_->failure = std::current_exception(); }
    }
  }
  void rethrow() const {
    std::lock_guard lock(state_->mutex);
    if (state_->failure) std::rethrow_exception(state_->failure);
  }
};
}
