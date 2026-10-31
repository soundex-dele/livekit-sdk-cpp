#pragma once
#include "livekit/rtc/room_engine.hpp"
#include "livekit/rtc/media_config.hpp"
#include "livekit/rtc/graph_types.hpp"
#include <map>
#include <atomic>
#include <mutex>

namespace livekit::sdk::rtc::engine {
class RtcGraphApi;
class MediaResources;
class MediaGraphBuilder;
class PublishPipeline;
class SubscribePipeline;

// Borrowed until set_observer(nullptr) returns. All public events run on notification.
class RtcObserver : public RoomObserver {
 public:
  ~RtcObserver() override = default;
};

// Three shared workers: control owns RoomEngine, lifecycle owns media graphs,
// notification delivers callbacks. Local request setup is synchronous; native
// results are asynchronous. Configure and leave from an external owner thread.
// Callbacks may submit requests, but must not wait for callbacks or destroy/leave
// the engine. Backend and media-library threads are unchanged.
class RtcEngine final : private RoomObserver {
 public:
  using TransportFactory = RoomEngine::TransportFactory;
  explicit RtcEngine(TransportFactory);
  ~RtcEngine();
  RtcEngine(const RtcEngine&) = delete;
  RtcEngine& operator=(const RtcEngine&) = delete;

  [[nodiscard]] ErrorCode set_observer(RtcObserver*);
  void join(RoomOptions, Completion<RoomSnapshot> = {});
  void leave(Completion<void> = {});
  void close(Completion<void> = {});
  RoomSnapshot snapshot() const;
  std::shared_ptr<RoomServices> services() const;

  [[nodiscard]] ErrorCode enable_media(AudioOutputOptions = {});
  bool media_enabled() const;
  void publish(TrackDescriptor, Completion<MediaPublication> = {});
  void subscribe(TrackDescriptor, Completion<MediaSubscription> = {});
  void subscribe(const TrackId&, Completion<MediaSubscription> = {});
  void publish_media(MediaPublishSpec, Completion<MediaPublication> = {});
  void publish_video(MediaPublishSpec, Completion<MediaPublication> = {});
  void publish_audio(MediaPublishSpec, Completion<MediaPublication> = {});
  void subscribe_media(MediaSubscribeSpec, Completion<MediaSubscription> = {});
  void unpublish(const TrackId&, Completion<void> = {});
  void unsubscribe(const TrackId&, Completion<void> = {});
  void set_muted(const TrackId&, bool, Completion<void> = {});
  void set_receive_demand(const TrackId&, ReceiveDemand, Completion<void> = {});
  void update_local_participant(ParticipantUpdate, Completion<void> = {});
  void set_subscription_permissions(SubscriptionPermissions, Completion<void> = {});
  Result<std::shared_ptr<SharedMediaInput>> create_shared_input(TrackDescriptor, MediaInput);
  RtcGraphApi graph();
  WriteStatus write(const TrackId&, EncodedFrame);
  TrackSnapshot publication(const TrackId&) const;
  Result<SenderFeedback> feedback(const TrackId&) const;

 private:
  friend class RtcGraphApi;
  void publish_graph(TrackDescriptor, PublishGraphFactory, Completion<MediaPublication> = {});
  void subscribe_graph(TrackDescriptor, SubscribeGraphFactory, Completion<MediaSubscription> = {});
  Result<void> clear_tracks();
  void on_published(TrackId, Result<std::shared_ptr<PublishedTrack>>, Completion<MediaPublication>);
  void on_subscribed(TrackId, Result<std::shared_ptr<SubscribedTrack>>,
                     Completion<MediaSubscription>);
  void on_unpublished(const TrackId&);
  void on_unsubscribed(const TrackId&);
  void on_room_event(const RoomEvent&) override;

  void notify(std::function<void(RtcObserver&)>);

  template <class T>
  void complete(Completion<T> callback, Result<T> result) {
    if (callback) {
      deliver([callback = std::move(callback), result = std::move(result)]() mutable {
        callback(std::move(result));
      });
    }
  }

  void deliver(std::function<void()>);
  std::shared_ptr<detail::RtcExecution> execution_;
  RtcObserver* observer_ = nullptr;  // notification worker
  std::atomic<bool> closing_{false};
  std::mutex leave_mutex_;
  mutable std::mutex ingress_mutex_;
  std::map<TrackId, std::shared_ptr<PublishPipeline>> ingress_;
  std::unique_ptr<RoomEngine> room_;
  std::unique_ptr<MediaResources> media_resources_;
  std::unique_ptr<MediaGraphBuilder> media_;

  struct Publication {
    PublishGraphFactory factory;
    std::optional<MediaPublishSpec> media;
    std::shared_ptr<PublishPipeline> pipeline;
    std::shared_ptr<MediaTrack> track;
  };

  struct Subscription {
    SubscribeGraphFactory factory;
    std::optional<MediaSubscribeSpec> media;
    std::shared_ptr<SubscribePipeline> pipeline;
    std::shared_ptr<MediaTrack> track;
  };

  // Graphs, factories and media resources belong to lifecycle.
  std::map<TrackId, Publication> publications_;
  std::map<TrackId, Subscription> subscriptions_;
};

std::unique_ptr<RtcEngine> create_livekit_engine();
Result<std::unique_ptr<RtcEngine>> create_livekit_engine(AudioOutputOptions);
}  // namespace livekit::sdk::rtc::engine
