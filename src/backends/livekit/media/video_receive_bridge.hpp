#pragma once
#include "livekit/client/types.hpp"
#include <mutex>

namespace livekit::sdk::client::detail {
struct TrackData;

// Per remote track. The native decoder and signaling thread share this route;
// generation invalidates completions when a subscription is detached/rebound.
struct VideoReceiveRoute {
  std::mutex mutex;
  std::weak_ptr<TrackData> track;
  uint64_t generation = 0, timestamp = 0;
  std::optional<uint32_t> previous;
};

// Per Room/native factory, never global. Reject ambiguous SSRC registrations
// rather than delivering a frame to a different track.
class VideoReceiveRoutes {
 public:
  bool add(uint32_t ssrc, const std::shared_ptr<VideoReceiveRoute>& route) {
    std::lock_guard lock(mutex_);
    auto existing = routes_[ssrc].lock();
    if (existing && existing != route) {
      return false;
    }
    routes_[ssrc] = route;
    return true;
  }

  void remove(uint32_t ssrc, const std::shared_ptr<VideoReceiveRoute>& route) {
    std::lock_guard lock(mutex_);
    auto found = routes_.find(ssrc);
    if (found != routes_.end() && found->second.lock() == route) {
      routes_.erase(found);
    }
  }

  std::shared_ptr<VideoReceiveRoute> find(uint32_t ssrc) {
    std::lock_guard lock(mutex_);
    auto found = routes_.find(ssrc);
    return found == routes_.end() ? nullptr : found->second.lock();
  }

 private:
  std::mutex mutex_;
  std::map<uint32_t, std::weak_ptr<VideoReceiveRoute>> routes_;
};
}  // namespace livekit::sdk::client::detail
