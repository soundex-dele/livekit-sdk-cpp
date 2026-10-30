#pragma once
#include "livekit/rtc/observer.hpp"
#include "livekit/rtc/media_config.hpp"

namespace livekit::sdk::rtc::engine {
struct GraphFailure {
  TrackId id;
  bool publication;
  std::string message;
  // Deduplicate with RoomEvent failures by failure->id.
  std::shared_ptr<const FailureInfo> failure;
};

class PublicationObserver {
 public:
  virtual ~PublicationObserver() = default;

  // Automatic codec replacement keeps TrackId but replaces the server SID and
  // MediaTrack handle. Failure leaves the old H265 publication stopped.
  virtual void on_video_codec_republished(const TrackId&, Result<MediaPublication>) {}

  virtual void on_publication_event(const RoomEvent&) {}

  virtual void on_graph_failure(const GraphFailure&) {}
};

class SubscriptionObserver {
 public:
  virtual ~SubscriptionObserver() = default;

  virtual void on_subscription_event(const RoomEvent&) {}

  virtual void on_graph_failure(const GraphFailure&) {}
};
}  // namespace livekit::sdk::rtc::engine
