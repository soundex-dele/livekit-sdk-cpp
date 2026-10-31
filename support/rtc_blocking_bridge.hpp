#pragma once
// Non-installed console/test adapter. Waits for per-call results, never drives RTC.
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/engine_observer.hpp"
#include "livekit/rtc/livekit.hpp"
#include "rtc_room_fixture.hpp"
#include "rtc_result.hpp"
#include <future>
#include <mutex>
#include <type_traits>

namespace livekit::sdk::app_support {
template <class Engine>
class BlockingEngine : private std::conditional_t<std::is_same_v<Engine, rtc::RoomEngine>,
                                                  rtc::RoomObserver, rtc::engine::RtcObserver> {
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
    (void)engine_->set_observer(this);
  }

  ~BlockingEngine() {
    if (engine_) {
      (void)engine_->set_observer(nullptr);
      // Drain accepted completions while adapter callback state is still alive.
      engine_.reset();
    }
  }

  Engine& raw() {
    return *engine_;
  }

  std::unique_ptr<Engine> release() {
    (void)engine_->set_observer(nullptr);
    return std::move(engine_);
  }

  ErrorCode set_event_handler(std::function<void(const rtc::RoomEvent&)> handler) {
    std::lock_guard lock(mutex_);
    callbacks_.on_event = std::move(handler);
    return ErrorCode::ok;
  }

  ErrorCode set_callbacks(Callbacks callbacks) {
    std::lock_guard lock(mutex_);
    callbacks_ = std::move(callbacks);
    return ErrorCode::ok;
  }

  ErrorCode join(rtc::RoomOptions options) {
    return code(await<rtc::RoomSnapshot>(
        [&](auto done) { engine_->join(std::move(options), std::move(done)); }));
  }

  ErrorCode leave() {
    return code(await<void>([&](auto done) { engine_->leave(std::move(done)); }));
  }

  ErrorCode close() {
    return leave();
  }

  auto publish(rtc::TrackDescriptor descriptor) {
    if constexpr (std::is_same_v<Engine, rtc::RoomEngine>) {
      return await<std::shared_ptr<rtc::PublishedTrack>>(
          [&](auto done) { engine_->publish(std::move(descriptor), std::move(done)); });
    } else {
      return track_id(await<rtc::engine::MediaPublication>(
          [&](auto done) { engine_->publish(std::move(descriptor), std::move(done)); }));
    }
  }

  auto subscribe(rtc::TrackDescriptor descriptor) {
    if constexpr (std::is_same_v<Engine, rtc::RoomEngine>) {
      return await<std::shared_ptr<rtc::SubscribedTrack>>(
          [&](auto done) { engine_->subscribe(std::move(descriptor), std::move(done)); });
    } else {
      return track_id(await<rtc::engine::MediaSubscription>(
          [&](auto done) { engine_->subscribe(std::move(descriptor), std::move(done)); }));
    }
  }

  ErrorCode unpublish(const rtc::TrackId& id) {
    return code(await<void>([&](auto done) { engine_->unpublish(id, std::move(done)); }));
  }

  ErrorCode unsubscribe(const rtc::TrackId& id) {
    return code(await<void>([&](auto done) { engine_->unsubscribe(id, std::move(done)); }));
  }

  ErrorCode set_muted(const rtc::TrackId& id, bool muted, rtc::ControlOptions = {}) {
    return code(await<void>([&](auto done) { engine_->set_muted(id, muted, std::move(done)); }));
  }

  ErrorCode set_receive_demand(const rtc::TrackId& id, rtc::ReceiveDemand demand,
                               rtc::ControlOptions = {}) {
    return code(
        await<void>([&](auto done) { engine_->set_receive_demand(id, demand, std::move(done)); }));
  }

  ErrorCode update_local_participant(rtc::ParticipantUpdate value, rtc::ControlOptions = {}) {
    return code(await<void>(
        [&](auto done) { engine_->update_local_participant(std::move(value), std::move(done)); }));
  }

  ErrorCode set_subscription_permissions(rtc::SubscriptionPermissions value,
                                         rtc::ControlOptions = {}) {
    return code(await<void>([&](auto done) {
      engine_->set_subscription_permissions(std::move(value), std::move(done));
    }));
  }

  Result<rtc::engine::MediaPublication> publish_media(rtc::engine::MediaPublishSpec spec) {
    return await<rtc::engine::MediaPublication>(
        [&](auto done) { engine_->publish_media(std::move(spec), std::move(done)); });
  }

  Result<rtc::engine::MediaPublication> publish_video(rtc::engine::MediaPublishSpec spec) {
    return await<rtc::engine::MediaPublication>(
        [&](auto done) { engine_->publish_video(std::move(spec), std::move(done)); });
  }

  Result<rtc::engine::MediaPublication> publish_audio(rtc::engine::MediaPublishSpec spec) {
    return await<rtc::engine::MediaPublication>(
        [&](auto done) { engine_->publish_audio(std::move(spec), std::move(done)); });
  }

  Result<rtc::engine::MediaSubscription> subscribe_media(rtc::engine::MediaSubscribeSpec spec) {
    return await<rtc::engine::MediaSubscription>(
        [&](auto done) { engine_->subscribe_media(std::move(spec), std::move(done)); });
  }

  ErrorCode enable_media(rtc::AudioOutputOptions options = {}) {
    return engine_->enable_media(std::move(options));
  }

  bool media_enabled() const {
    return engine_->media_enabled();
  }

  auto create_shared_input(rtc::TrackDescriptor format, rtc::engine::MediaInput input) {
    return engine_->create_shared_input(std::move(format), std::move(input));
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
      return owner_.track_id(owner_.template await<rtc::engine::MediaPublication>([&](auto done) {
        graph_.publish(std::move(d), std::forward<Args>(args)..., std::move(done));
      }));
    }

    template <class... Args>
    Result<rtc::TrackId> subscribe(rtc::TrackDescriptor d, Args&&... args) {
      return owner_.track_id(owner_.template await<rtc::engine::MediaSubscription>([&](auto done) {
        graph_.subscribe(std::move(d), std::forward<Args>(args)..., std::move(done));
      }));
    }

    template <class... Args>
    Result<rtc::TrackId> publish_graph(rtc::TrackDescriptor d, Args&&... args) {
      return owner_.track_id(owner_.template await<rtc::engine::MediaPublication>([&](auto done) {
        graph_.publish_graph(std::move(d), std::forward<Args>(args)..., std::move(done));
      }));
    }

    template <class... Args>
    Result<rtc::TrackId> subscribe_graph(rtc::TrackDescriptor d, Args&&... args) {
      return owner_.track_id(owner_.template await<rtc::engine::MediaSubscription>([&](auto done) {
        graph_.subscribe_graph(std::move(d), std::forward<Args>(args)..., std::move(done));
      }));
    }

   private:
    BlockingEngine& owner_;
    Graph graph_;
  };

  auto graph() {
    return GraphBridge<decltype(engine_->graph())>(*this, engine_->graph());
  }

 private:
  template <class T>
  static Result<T> failure(ErrorCode code, const char* message) {
    return Result<T>::failure(std::make_shared<FailureInfo>(
        FailureInfo{next_failure_id(), ErrorDomain::lifecycle, code, message}));
  }

  template <class T, class Submit>
  Result<T> await(Submit submit) {
    if (rtc::is_callback_thread()) {
      return failure<T>(ErrorCode::invalid_thread, "Cannot block inside RTC callbacks");
    }
    auto promise = std::make_shared<std::promise<Result<T>>>();
    auto future = promise->get_future();
    submit([this, promise](Result<T> result) {
      if constexpr (std::is_same_v<T, rtc::engine::MediaPublication> ||
                    std::is_same_v<T, rtc::engine::MediaSubscription>) {
        if (result) {
          rtc::RoomEvent event{rtc::RoomEventKind::track_ready, result->id};
          event.publication = std::is_same_v<T, rtc::engine::MediaPublication>;
          on_room_event(event);
        }
      }
      promise->set_value(std::move(result));
    });
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
      return failure<T>(ErrorCode::timeout, "Application wait expired");
    }
    return future.get();
  }

  template <class T>
  static ErrorCode code(const Result<T>& result) {
    return result ? ErrorCode::ok : result.error().code;
  }

  template <class T>
  static Result<rtc::TrackId> track_id(Result<T> result) {
    return result ? Result<rtc::TrackId>::success(result->id)
                  : Result<rtc::TrackId>::failure(result.failure_record());
  }

  void on_room_event(const rtc::RoomEvent& event) override {
    Callbacks callbacks;
    {
      std::lock_guard lock(mutex_);
      callbacks = callbacks_;
    }
    if (callbacks.on_event) {
      callbacks.on_event(event);
    }
    if (event.kind == rtc::RoomEventKind::track_failed && callbacks.on_failure) {
      callbacks.on_failure(
          {event.track_id, event.publication.value_or(false), event.message, event.failure});
    }
  }

  std::mutex mutex_;
  std::unique_ptr<Engine> engine_;
  Callbacks callbacks_;
};

using BlockingRoom = BlockingEngine<rtc::RoomEngine>;
using BlockingRtc = BlockingEngine<rtc::engine::RtcEngine>;

inline Result<std::unique_ptr<BlockingRtc>> create_blocking_engine() {
  return Result<std::unique_ptr<BlockingRtc>>::success(
      std::make_unique<BlockingRtc>(rtc::engine::create_livekit_engine()));
}

inline Result<std::unique_ptr<BlockingRtc>> create_blocking_engine(
    rtc::AudioOutputOptions options) {
  auto engine = rtc::engine::create_livekit_engine();
  auto code = engine->enable_media(std::move(options));
  if (code != ErrorCode::ok) {
    return Result<std::unique_ptr<BlockingRtc>>::failure(std::make_shared<FailureInfo>(
        FailureInfo{next_failure_id(), ErrorDomain::media, code, "Media setup failed"}));
  }
  return Result<std::unique_ptr<BlockingRtc>>::success(
      std::make_unique<BlockingRtc>(std::move(engine)));
}
}  // namespace livekit::sdk::app_support
