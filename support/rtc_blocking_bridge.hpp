#pragma once
// Non-installed adapter for older console supervisors. The SDK itself exposes
// only void controls and callbacks. This helper waits for backend notifications;
// it never drives the engine or creates a worker.
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/engine_observer.hpp"
#include "livekit/rtc/livekit.hpp"
#include "rtc_room_fixture.hpp"
#include "rtc_result.hpp"
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <set>

namespace livekit::sdk::app_support {
template <class Engine>
class BlockingEngine : private rtc::RoomObserver {
 public:
  using TransportFactory = rtc::RoomEngine::TransportFactory;
  using GraphFailure = rtc::engine::GraphFailure;

  struct Callbacks {
    std::function<void(const rtc::RoomEvent&)> on_event;
    std::function<void(const GraphFailure&)> on_failure;
  };

  explicit BlockingEngine(TransportFactory factory)
      : BlockingEngine(std::make_unique<Engine>(std::move(factory))) {}

  explicit BlockingEngine(std::unique_ptr<Engine> engine) : engine_(std::move(engine)) {
    if constexpr (std::is_same_v<Engine, rtc::RoomEngine>) {
      engine_->set_observer(this);
    } else {
      rtc::engine::RtcCallbacks callbacks;
      callbacks.on_joined = [this](const auto& value) { on_joined(value); };
      callbacks.on_left = [this] { on_left(); };
      callbacks.on_published = [this](const rtc::engine::MediaPublication& value) {
        {
          std::lock_guard lock(mutex_);
          media_publications_[value.id] = value;
        }
        emit_track(rtc::RoomEventKind::track_ready, value.id, true);
      };
      callbacks.on_subscribed = [this](const rtc::engine::MediaSubscription& value) {
        {
          std::lock_guard lock(mutex_);
          media_subscriptions_[value.id] = value;
        }
        emit_track(rtc::RoomEventKind::track_ready, value.id, false);
      };
      callbacks.on_unpublished = [this](const auto& id) { on_unpublished(id); };
      callbacks.on_unsubscribed = [this](const auto& id) { on_unsubscribed(id); };
      callbacks.on_event = [this](const auto& event) { on_room_event(event); };
      engine_->set_callbacks(std::move(callbacks));
    }
  }

  ~BlockingEngine() {
    if (engine_) {
      if constexpr (std::is_same_v<Engine, rtc::RoomEngine>) {
        engine_->set_observer(nullptr);
      } else {
        engine_->set_callbacks({});
      }
    }
  }

  Engine& raw() {
    return *engine_;
  }

  std::unique_ptr<Engine> release() {
    if constexpr (std::is_same_v<Engine, rtc::RoomEngine>) {
      engine_->set_observer(nullptr);
    } else {
      engine_->set_callbacks({});
    }
    return std::move(engine_);
  }

  ErrorCode set_event_handler(std::function<void(const rtc::RoomEvent&)> handler) {
    callbacks_.on_event = std::move(handler);
    return ErrorCode::ok;
  }

  ErrorCode set_callbacks(Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
    return ErrorCode::ok;
  }

  ErrorCode join(rtc::RoomOptions options) {
    joined_ = false;
    engine_->join(std::move(options));
    return wait([&] { return joined_; });
  }

  ErrorCode leave() {
    engine_->leave();
    return ErrorCode::ok;
  }

  ErrorCode close() {
    return leave();
  }

  auto publish(rtc::TrackDescriptor descriptor) {
    const auto id = descriptor.id;
    engine_->publish(std::move(descriptor));
    if constexpr (std::is_same_v<Engine, rtc::RoomEngine>) {
      return result<std::shared_ptr<rtc::PublishedTrack>>(
          [&] { return publications_.contains(id); }, [&] { return publications_.at(id); });
    } else {
      return result<rtc::TrackId>([&] { return media_publications_.contains(id); },
                                  [&] { return id; });
    }
  }

  auto subscribe(rtc::TrackDescriptor descriptor) {
    const auto id = descriptor.id;
    engine_->subscribe(std::move(descriptor));
    if constexpr (std::is_same_v<Engine, rtc::RoomEngine>) {
      return result<std::shared_ptr<rtc::SubscribedTrack>>(
          [&] { return subscriptions_.contains(id); }, [&] { return subscriptions_.at(id); });
    } else {
      return result<rtc::TrackId>([&] { return media_subscriptions_.contains(id); },
                                  [&] { return id; });
    }
  }

  ErrorCode unpublish(const rtc::TrackId& id) {
    removed_publications_.erase(id);
    engine_->unpublish(id);
    return wait([&] { return removed_publications_.contains(id); });
  }

  ErrorCode unsubscribe(const rtc::TrackId& id) {
    removed_subscriptions_.erase(id);
    engine_->unsubscribe(id);
    return wait([&] { return removed_subscriptions_.contains(id); });
  }

  ErrorCode set_muted(const rtc::TrackId& id, bool muted, rtc::ControlOptions = {}) {
    engine_->set_muted(id, muted);
    return ErrorCode::ok;
  }

  ErrorCode set_receive_demand(const rtc::TrackId& id, rtc::ReceiveDemand demand,
                               rtc::ControlOptions = {}) {
    engine_->set_receive_demand(id, demand);
    return ErrorCode::ok;
  }

  ErrorCode update_local_participant(rtc::ParticipantUpdate value, rtc::ControlOptions = {}) {
    engine_->update_local_participant(std::move(value));
    return ErrorCode::ok;
  }

  ErrorCode set_subscription_permissions(rtc::SubscriptionPermissions value,
                                         rtc::ControlOptions = {}) {
    engine_->set_subscription_permissions(std::move(value));
    return ErrorCode::ok;
  }

  Result<rtc::engine::MediaPublication> publish_media(rtc::engine::MediaPublishSpec spec) {
    const auto id = spec.track.id;
    engine_->publish_media(std::move(spec));
    return result<rtc::engine::MediaPublication>([&] { return media_publications_.contains(id); },
                                                 [&] { return media_publications_.at(id); });
  }

  Result<rtc::engine::MediaPublication> publish_video(rtc::engine::MediaPublishSpec spec) {
    const auto id = spec.track.id;
    engine_->publish_video(std::move(spec));
    return result<rtc::engine::MediaPublication>([&] { return media_publications_.contains(id); },
                                                 [&] { return media_publications_.at(id); });
  }

  Result<rtc::engine::MediaPublication> publish_audio(rtc::engine::MediaPublishSpec spec) {
    const auto id = spec.track.id;
    engine_->publish_audio(std::move(spec));
    return result<rtc::engine::MediaPublication>([&] { return media_publications_.contains(id); },
                                                 [&] { return media_publications_.at(id); });
  }

  Result<rtc::engine::MediaSubscription> subscribe_media(rtc::engine::MediaSubscribeSpec spec) {
    const auto id = spec.track.id;
    engine_->subscribe_media(std::move(spec));
    return result<rtc::engine::MediaSubscription>([&] { return media_subscriptions_.contains(id); },
                                                  [&] { return media_subscriptions_.at(id); });
  }

  ErrorCode enable_media(rtc::AudioOutputOptions options = {}) {
    engine_->enable_media(std::move(options));
    return ErrorCode::ok;
  }

  bool media_enabled() const {
    return engine_->media_enabled();
  }

  auto create_shared_input(rtc::TrackDescriptor format, rtc::engine::MediaInput input) {
    return Result<std::shared_ptr<rtc::engine::SharedMediaInput>>::success(
        engine_->create_shared_input(std::move(format), std::move(input)));
  }

  auto snapshot() const {
    return engine_->snapshot();
  }

  auto services() const {
    return Result<std::shared_ptr<rtc::RoomServices>>::success(engine_->services());
  }

  BlockingEngine& room() {
    return *this;
  }

  auto publication(const rtc::TrackId& id) const {
    return engine_->publication(id);
  }

  auto feedback(const rtc::TrackId& id) const {
    return engine_->feedback(id);
  }

  auto write(const rtc::TrackId& id, rtc::EncodedFrame frame) {
    return engine_->write(id, std::move(frame));
  }

  template <class Graph>
  class GraphBridge {
   public:
    GraphBridge(BlockingEngine& owner, Graph graph) : owner_(owner), graph_(std::move(graph)) {}

    template <class... Args>
    Result<rtc::TrackId> publish(rtc::TrackDescriptor d, Args&&... args) {
      const auto id = d.id;
      graph_.publish(std::move(d), std::forward<Args>(args)...);
      return owner_.template result<rtc::TrackId>(
          [&] { return owner_.media_publications_.contains(id); }, [&] { return id; });
    }

    template <class... Args>
    Result<rtc::TrackId> subscribe(rtc::TrackDescriptor d, Args&&... args) {
      const auto id = d.id;
      graph_.subscribe(std::move(d), std::forward<Args>(args)...);
      return owner_.template result<rtc::TrackId>(
          [&] { return owner_.media_subscriptions_.contains(id); }, [&] { return id; });
    }

    template <class... Args>
    Result<rtc::TrackId> publish_graph(rtc::TrackDescriptor d, Args&&... args) {
      const auto id = d.id;
      graph_.publish_graph(std::move(d), std::forward<Args>(args)...);
      return owner_.template result<rtc::TrackId>(
          [&] { return owner_.media_publications_.contains(id); }, [&] { return id; });
    }

    template <class... Args>
    Result<rtc::TrackId> subscribe_graph(rtc::TrackDescriptor d, Args&&... args) {
      const auto id = d.id;
      graph_.subscribe_graph(std::move(d), std::forward<Args>(args)...);
      return owner_.template result<rtc::TrackId>(
          [&] { return owner_.media_subscriptions_.contains(id); }, [&] { return id; });
    }

   private:
    BlockingEngine& owner_;
    Graph graph_;
  };

  auto graph() {
    return GraphBridge<decltype(engine_->graph())>(*this, engine_->graph());
  }

 private:
  template <class Predicate>
  ErrorCode wait(Predicate predicate) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(30), predicate) ? ErrorCode::ok
                                                                        : ErrorCode::timeout;
  }

  template <class T, class Predicate, class Value>
  Result<T> result(Predicate predicate, Value value) {
    if (wait(predicate) != ErrorCode::ok) {
      return Result<T>::failure(std::make_shared<FailureInfo>(
          FailureInfo{next_failure_id(), ErrorDomain::lifecycle, ErrorCode::timeout,
                      "Application wait expired"}));
    }
    std::lock_guard lock(mutex_);
    return Result<T>::success(value());
  }

  void emit_track(rtc::RoomEventKind kind, const rtc::TrackId& id, bool publication) {
    rtc::RoomEvent event{kind, id};
    event.publication = publication;
    on_room_event(event);
  }

  void on_joined(const rtc::RoomSnapshot&) override {
    {
      std::lock_guard lock(mutex_);
      joined_ = true;
    }
    on_room_event({rtc::RoomEventKind::connected});
  }

  void on_left() override {
    {
      std::lock_guard lock(mutex_);
      joined_ = false;
      publications_.clear();
      subscriptions_.clear();
      media_publications_.clear();
      media_subscriptions_.clear();
    }
    on_room_event({rtc::RoomEventKind::closed});
  }

  void on_published(std::shared_ptr<rtc::PublishedTrack> track) override {
    {
      std::lock_guard lock(mutex_);
      publications_[track->descriptor().id] = track;
    }
    emit_track(rtc::RoomEventKind::track_ready, track->descriptor().id, true);
  }

  void on_subscribed(std::shared_ptr<rtc::SubscribedTrack> track) override {
    {
      std::lock_guard lock(mutex_);
      subscriptions_[track->descriptor().id] = track;
    }
    emit_track(rtc::RoomEventKind::track_ready, track->descriptor().id, false);
  }

  void on_unpublished(const rtc::TrackId& id) override {
    {
      std::lock_guard lock(mutex_);
      removed_publications_.insert(id);
      publications_.erase(id);
      media_publications_.erase(id);
    }
    emit_track(rtc::RoomEventKind::track_removed, id, true);
  }

  void on_unsubscribed(const rtc::TrackId& id) override {
    {
      std::lock_guard lock(mutex_);
      removed_subscriptions_.insert(id);
      subscriptions_.erase(id);
      media_subscriptions_.erase(id);
    }
    emit_track(rtc::RoomEventKind::track_removed, id, false);
  }

  void on_room_event(const rtc::RoomEvent& event) override {
    changed_.notify_all();
    if (callbacks_.on_event) {
      callbacks_.on_event(event);
    }
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  std::unique_ptr<Engine> engine_;
  Callbacks callbacks_;
  bool joined_ = false;
  std::map<rtc::TrackId, std::shared_ptr<rtc::PublishedTrack>> publications_;
  std::map<rtc::TrackId, std::shared_ptr<rtc::SubscribedTrack>> subscriptions_;
  std::map<rtc::TrackId, rtc::engine::MediaPublication> media_publications_;
  std::map<rtc::TrackId, rtc::engine::MediaSubscription> media_subscriptions_;
  std::set<rtc::TrackId> removed_publications_, removed_subscriptions_;
};

using BlockingRoom = BlockingEngine<rtc::RoomEngine>;
using BlockingRtc = BlockingEngine<rtc::engine::RtcEngine>;

inline Result<std::unique_ptr<BlockingRtc>> create_blocking_engine() {
  return Result<std::unique_ptr<BlockingRtc>>::success(
      std::make_unique<BlockingRtc>(rtc::engine::create_livekit_engine()));
}

inline Result<std::unique_ptr<BlockingRtc>> create_blocking_engine(
    rtc::AudioOutputOptions options) {
  return Result<std::unique_ptr<BlockingRtc>>::success(
      std::make_unique<BlockingRtc>(rtc::engine::create_livekit_engine(std::move(options))));
}
}  // namespace livekit::sdk::app_support
