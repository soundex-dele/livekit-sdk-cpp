#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <utility>

namespace livekit::sdk::rtc::detail {

enum class EventAdmission { queued, coalesced, dropped, overflow };

// Caller owns synchronization and terminal-overflow handling. Both room and
// transport events keep the latest statistics per target and give control events
// priority. State-event coalescing remains specific to each boundary.
template <class Event, class Kind, class Coalesce>
EventAdmission enqueue_event(std::deque<Event>& queue, Event event, size_t capacity,
                             Kind statistics_kind, Coalesce coalesce) {
  if (event.kind == statistics_kind) {
    for (auto& queued : queue) {
      if (queued.kind == statistics_kind && queued.statistics->target == event.statistics->target) {
        queued = std::move(event);
        return EventAdmission::coalesced;
      }
    }
    if (queue.size() >= capacity) {
      return EventAdmission::dropped;
    }
  } else if (queue.size() >= capacity) {
    const auto stale = std::find_if(queue.begin(), queue.end(), [&](const auto& queued) {
      return queued.kind == statistics_kind;
    });
    if (stale != queue.end()) {
      queue.erase(stale);
    }
  }
  if (coalesce(queue, event)) {
    return EventAdmission::coalesced;
  }
  if (queue.size() >= capacity) {
    return EventAdmission::overflow;
  }
  queue.push_back(std::move(event));
  return EventAdmission::queued;
}

}  // namespace livekit::sdk::rtc::detail
