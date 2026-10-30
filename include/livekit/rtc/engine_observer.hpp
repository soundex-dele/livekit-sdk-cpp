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

  virtual void on_publish_completed(const TrackId&, Result<TrackId>) {}

  virtual void on_shared_input_created(const TrackId&, Result<std::shared_ptr<SharedMediaInput>>) {}

  virtual void on_media_publish_completed(const TrackId&, Result<MediaPublication>) {}

  // Automatic codec replacement keeps TrackId but replaces the server SID and
  // MediaTrack handle. Failure leaves the old H265 publication stopped.
  virtual void on_video_codec_republished(const TrackId&, Result<MediaPublication>) {}

  virtual void on_unpublish_completed(const TrackId&, const OperationResult&) {}

  virtual void on_muted_completed(const TrackId&, bool, const OperationResult&) {}

  virtual void on_publication_event(const RoomEvent&) {}

  virtual void on_graph_failure(const GraphFailure&) {}
};

class SubscriptionObserver {
 public:
  virtual ~SubscriptionObserver() = default;

  virtual void on_subscribe_completed(const TrackId&, Result<TrackId>) {}

  virtual void on_media_subscribe_completed(const TrackId&, Result<MediaSubscription>) {}

  virtual void on_unsubscribe_completed(const TrackId&, const OperationResult&) {}

  virtual void on_receive_demand_completed(const TrackId&, ReceiveDemand, const OperationResult&) {}

  virtual void on_subscription_event(const RoomEvent&) {}

  virtual void on_graph_failure(const GraphFailure&) {}
};
}  // namespace livekit::sdk::rtc::engine
