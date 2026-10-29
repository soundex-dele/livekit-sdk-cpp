#pragma once
// Application/test adapter for session loops. The SDK delivers callbacks;
// this bounded mailbox is not installed as part of its public API.
#include "livekit/rtc/room_services.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <type_traits>
#include <chrono>
#include <optional>
#include <stdexcept>

namespace livekit::sdk::app_support {
class DataEventMailbox {
  struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<rtc::DataEvent> events;
    size_t bytes = 0;
    bool overflow = false;
  };
  std::shared_ptr<State> state_ = std::make_shared<State>();
  std::shared_ptr<rtc::RoomServices> services_;

  static size_t event_bytes(const rtc::DataEvent& event) {
    size_t bytes = event.sender_identity.size();
    std::visit([&](const auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, rtc::ReceivedData>) {
        bytes += value.topic.size() + value.bytes.size();
      } else if constexpr (std::is_same_v<T, rtc::DataServiceError>) {
        bytes += value.message.size();
      } else if constexpr (std::is_same_v<T, Transcription>) {
        bytes += value.participant_identity.size() + value.track_id.size();
        for (const auto& segment : value.segments) {
          bytes += segment.id.size() + segment.text.size() + segment.language.size();
        }
      } else { bytes += value.digit.size(); }
    }, event.value);
    return bytes;
  }

 public:
  explicit DataEventMailbox(std::shared_ptr<rtc::RoomServices> services)
      : services_(std::move(services)) {
    services_->set_event_handler([weak = std::weak_ptr<State>(state_)](const auto& event) {
      if (auto state = weak.lock()) {
        const auto bytes = event_bytes(event);
        std::lock_guard lock(state->mutex);
        if (state->events.size() >= 256 || bytes > 4 * 1024 * 1024 - state->bytes) {
          state->overflow = true;
        } else {
          state->events.push_back(event);
          state->bytes += bytes;
        }
        state->changed.notify_all();
      }
    });
  }
  ~DataEventMailbox() { services_->set_event_handler({}); }
  DataEventMailbox(const DataEventMailbox&) = delete;
  DataEventMailbox& operator=(const DataEventMailbox&) = delete;

  std::optional<rtc::DataEvent> take_event(std::chrono::milliseconds wait = {}) {
    std::unique_lock lock(state_->mutex);
    state_->changed.wait_for(lock, wait, [&] { return state_->overflow || !state_->events.empty(); });
    if (state_->overflow) { throw std::runtime_error("Application data event mailbox exhausted"); }
    if (state_->events.empty()) { return {}; }
    auto event = std::move(state_->events.front());
    state_->events.pop_front();
    state_->bytes -= event_bytes(event);
    return event;
  }
};
}  // namespace livekit::sdk::app_support
