#pragma once
#include "livekit/rtc/room_engine.hpp"
#include "livekit/rtc/media_config.hpp"
#include "livekit/rtc/graph_types.hpp"
#include <map>

namespace livekit::sdk::rtc::engine {
class RtcGraphApi;
class MediaResources;
class MediaGraphBuilder;
class PublishPipeline;
class SubscribePipeline;

struct RtcCallbacks {
  std::function<void(const RoomSnapshot&)> on_joined;
  std::function<void()> on_left;
  std::function<void(const TrackDescriptor&)> on_track_published;
  std::function<void(const MediaPublication&)> on_published;
  std::function<void(const MediaSubscription&)> on_subscribed;
  std::function<void(const TrackId&)> on_unpublished;
  std::function<void(const TrackId&)> on_unsubscribed;
  std::function<void(const RoomEvent&)> on_event;
};

// Single-room facade. The backend pushes success callbacks after media graph
// startup; the application does not drive an event pump.
// No control executors, locks, per-call completions or exception handling.
// Callbacks run on the notifying backend thread (leave on its caller). Do not
// wait for another result, leave or destroy the engine from a backend callback.
class RtcEngine final : private RoomObserver {
 public:
  using TransportFactory = RoomEngine::TransportFactory;
  explicit RtcEngine(TransportFactory);
  ~RtcEngine();
  RtcEngine(const RtcEngine&) = delete;
  RtcEngine& operator=(const RtcEngine&) = delete;

  void set_callbacks(RtcCallbacks);
  void join(RoomOptions);
  void leave();
  void close();
  RoomSnapshot snapshot() const;
  std::shared_ptr<RoomServices> services() const;

  void enable_media(AudioOutputOptions = {});
  bool media_enabled() const;
  void publish(TrackDescriptor);
  void subscribe(TrackDescriptor);
  void subscribe(const TrackId&);
  void publish_media(MediaPublishSpec);
  void publish_video(MediaPublishSpec);
  void publish_audio(MediaPublishSpec);
  void subscribe_media(MediaSubscribeSpec);
  void unpublish(const TrackId&);
  void unsubscribe(const TrackId&);
  void set_muted(const TrackId&, bool);
  void set_receive_demand(const TrackId&, ReceiveDemand);
  void update_local_participant(ParticipantUpdate);
  void set_subscription_permissions(SubscriptionPermissions);
  std::shared_ptr<SharedMediaInput> create_shared_input(TrackDescriptor, MediaInput);
  RtcGraphApi graph();
  WriteStatus write(const TrackId&, EncodedFrame);
  TrackSnapshot publication(const TrackId&) const;
  SenderFeedback feedback(const TrackId&) const;

 private:
  friend class RtcGraphApi;
  void publish_graph(TrackDescriptor, PublishGraphFactory);
  void subscribe_graph(TrackDescriptor, SubscribeGraphFactory);
  void clear_tracks();
  void on_joined(const RoomSnapshot&) override;
  void on_left() override;
  void on_published(std::shared_ptr<PublishedTrack>) override;
  void on_subscribed(std::shared_ptr<SubscribedTrack>) override;
  void on_unpublished(const TrackId&) override;
  void on_unsubscribed(const TrackId&) override;
  void on_room_event(const RoomEvent&) override;

  RtcCallbacks callbacks_;
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

  std::map<TrackId, Publication> publications_;
  std::map<TrackId, Subscription> subscriptions_;
};

std::unique_ptr<RtcEngine> create_livekit_engine();
std::unique_ptr<RtcEngine> create_livekit_engine(AudioOutputOptions);
}  // namespace livekit::sdk::rtc::engine
