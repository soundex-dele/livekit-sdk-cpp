#pragma once
// Non-installed bridge for existing blocking CLI/test supervisors. The SDK API
// uses per-call completions; application events still use observers.
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/livekit.hpp"
#include "rtc_result.hpp"
#include <future>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <type_traits>

namespace livekit::sdk::app_support {
inline thread_local bool in_blocking_bridge_callback = false;

struct BridgeCallbackScope {
  bool previous = std::exchange(in_blocking_bridge_callback, true);

  ~BridgeCallbackScope() {
    in_blocking_bridge_callback = previous;
  }
};

// The blocking adapter must also reject waits from media callbacks. A close
// waits for these callbacks to finish, so waiting for close here would deadlock.
template <class Value, class... Args>
void guard_bridge_callback(std::function<Value(Args...)>& callback) {
  if (callback) {
    callback = [handler = std::move(callback)](Args... args) -> Value {
      BridgeCallbackScope scope;
      return handler(std::forward<Args>(args)...);
    };
  }
}

inline void guard_media_callbacks(rtc::engine::MediaOutput& output) {
  guard_bridge_callback(output.video);
  guard_bridge_callback(output.audio);
  guard_bridge_callback(output.frame);
}

inline void guard_media_callbacks(rtc::engine::MediaPublishSpec& spec) {
  guard_media_callbacks(spec.preview);
  guard_bridge_callback(spec.h264_fallback_input);
}

struct BlockingObservers : rtc::RoomObserver,
                           rtc::PublicationObserver,
                           rtc::SubscriptionObserver,
                           rtc::engine::PublicationObserver,
                           rtc::engine::SubscriptionObserver {
  std::mutex mutex;
  std::condition_variable changed;
  std::function<void(const rtc::RoomEvent&)> event;
  std::function<void(const rtc::engine::GraphFailure&)> failure;
  size_t invoking = 0;

  template <class T>
  static T rejected(ErrorCode code) {
    if constexpr (std::is_same_v<T, ErrorCode>) {
      return code;
    } else {
      return T::failure(std::make_shared<FailureInfo>(
          FailureInfo{next_failure_id(), ErrorDomain::lifecycle, code, "RTC request rejected"}));
    }
  }

  template <class T, class Submit>
  T wait(const std::string&, Submit submit) {
    if (in_blocking_bridge_callback || rtc::is_callback_thread()) {
      return rejected<T>(ErrorCode::invalid_thread);
    }
    using Completed = std::conditional_t<std::is_same_v<T, ErrorCode>, Result<void>, T>;
    auto promise = std::make_shared<std::promise<Completed>>();
    auto pending = promise->get_future();
    auto code = submit([promise](Completed result) { promise->set_value(std::move(result)); });
    promise.reset();
    if (code != ErrorCode::ok) {
      return rejected<T>(code);
    }
    try {
      auto result = pending.get();
      if constexpr (std::is_same_v<T, ErrorCode>) {
        return result ? ErrorCode::ok : result.error().code;
      } else {
        return result;
      }
    } catch (const std::future_error&) {
      return rejected<T>(ErrorCode::callback_failed);
    }
  }

  ErrorCode set_handlers(std::function<void(const rtc::RoomEvent&)> e,
                         std::function<void(const rtc::engine::GraphFailure&)> f) {
    if (in_blocking_bridge_callback || rtc::is_callback_thread()) {
      return ErrorCode::invalid_thread;
    }
    std::unique_lock lock(mutex);
    event = std::move(e);
    failure = std::move(f);
    changed.wait(lock, [&] { return invoking == 0; });
    return ErrorCode::ok;
  }

  template <class Function, class Value>
  void invoke(Function handler, const Value& value) {
    if (!handler) {
      return;
    }
    try {
      BridgeCallbackScope scope;
      handler(value);
    } catch (...) {
      std::lock_guard lock(mutex);
      --invoking;
      event = {};
      failure = {};
      changed.notify_all();
      throw;
    }
    std::lock_guard lock(mutex);
    --invoking;
    changed.notify_all();
  }

  void on_room_event(const rtc::RoomEvent& value) override {
    std::function<void(const rtc::RoomEvent&)> handler;
    {
      std::lock_guard lock(mutex);
      handler = event;
      if (handler) {
        ++invoking;
      }
    }
    invoke(std::move(handler), value);
  }

  void on_publication_event(const rtc::RoomEvent& value) override {
    on_room_event(value);
  }

  void on_subscription_event(const rtc::RoomEvent& value) override {
    on_room_event(value);
  }

  void on_graph_failure(const rtc::engine::GraphFailure& value) override {
    std::function<void(const rtc::engine::GraphFailure&)> handler;
    {
      std::lock_guard lock(mutex);
      handler = failure;
      if (handler) {
        ++invoking;
      }
    }
    invoke(std::move(handler), value);
  }


};

template <class Engine>
class BlockingEngine {
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
    engine_->set_room_observer(std::static_pointer_cast<rtc::RoomObserver>(observer_));
    if constexpr (std::is_same_v<Engine, rtc::RoomEngine>) {
      engine_->set_publication_observer(
          std::static_pointer_cast<rtc::PublicationObserver>(observer_));
      engine_->set_subscription_observer(
          std::static_pointer_cast<rtc::SubscriptionObserver>(observer_));
    } else {
      engine_->set_publication_observer(
          std::static_pointer_cast<rtc::engine::PublicationObserver>(observer_));
      engine_->set_subscription_observer(
          std::static_pointer_cast<rtc::engine::SubscriptionObserver>(observer_));
    }
  }

  explicit BlockingEngine(std::unique_ptr<BlockingEngine<rtc::RoomEngine>> room)
    requires std::is_same_v<Engine, rtc::engine::RtcEngine>
      : BlockingEngine(std::make_unique<Engine>(room ? room->release() : nullptr)) {}

  BlockingEngine(const BlockingEngine&) = delete;
  BlockingEngine& operator=(const BlockingEngine&) = delete;

  Engine& raw() {
    return *engine_;
  }

  std::unique_ptr<Engine> release() {
    return std::move(engine_);
  }

  ErrorCode set_event_handler(std::function<void(const rtc::RoomEvent&)> event) {
    return observer_->set_handlers(std::move(event), {});
  }

  ErrorCode set_callbacks(Callbacks callbacks) {
    return observer_->set_handlers(std::move(callbacks.on_event), std::move(callbacks.on_failure));
  }

  ErrorCode join(rtc::RoomOptions options) {
    return observer_->template wait<ErrorCode>("join", [&](auto completion) {
      return engine_->join(std::move(options), std::move(completion));
    });
  }

  ErrorCode leave() {
    return observer_->template wait<ErrorCode>(
        "leave", [&](auto completion) { return engine_->leave(std::move(completion)); });
  }

  ErrorCode close() {
    if (in_blocking_bridge_callback || rtc::is_callback_thread()) {
      return ErrorCode::invalid_thread;
    }
    if (closed_) {
      return ErrorCode::ok;
    }
    auto code = observer_->template wait<ErrorCode>(
        "close", [&](auto completion) { return engine_->close(std::move(completion)); });
    if (code == ErrorCode::ok) {
      closed_ = true;
    }
    return code;
  }

  auto publish(rtc::TrackDescriptor descriptor) {
    using Value = std::conditional_t<std::is_same_v<Engine, rtc::RoomEngine>,
                                     std::shared_ptr<rtc::PublishedTrack>, rtc::TrackId>;
    const auto id = descriptor.id;
    return observer_->template wait<Result<Value>>("publish:" + id, [&](auto completion) {
      return engine_->publish(std::move(descriptor), std::move(completion));
    });
  }

  auto subscribe(rtc::TrackDescriptor descriptor) {
    const auto id = descriptor.id;
    return observer_->template wait<Result<std::shared_ptr<rtc::SubscribedTrack>>>(
        "subscribe:" + id, [&](auto completion) {
          return engine_->subscribe(std::move(descriptor), std::move(completion));
        });
  }

  ErrorCode unpublish(const rtc::TrackId& id) {
    return observer_->template wait<ErrorCode>("unpublish:" + id, [&](auto completion) {
      return engine_->unpublish(id, std::move(completion));
    });
  }

  ErrorCode unsubscribe(const rtc::TrackId& id) {
    return observer_->template wait<ErrorCode>("unsubscribe:" + id, [&](auto completion) {
      return engine_->unsubscribe(id, std::move(completion));
    });
  }

  ErrorCode set_muted(const rtc::TrackId& id, bool muted, rtc::ControlOptions options = {}) {
    return observer_->template wait<ErrorCode>("mute:" + id, [&](auto completion) {
      return engine_->set_muted(id, muted, std::move(completion), options);
    });
  }

  ErrorCode set_receive_demand(const rtc::TrackId& id, rtc::ReceiveDemand demand,
                               rtc::ControlOptions options = {}) {
    return observer_->template wait<ErrorCode>("demand:" + id, [&](auto completion) {
      return engine_->set_receive_demand(id, demand, std::move(completion), options);
    });
  }

  ErrorCode update_local_participant(rtc::ParticipantUpdate value,
                                     rtc::ControlOptions options = {}) {
    return observer_->template wait<ErrorCode>("participant", [&](auto completion) {
      return room_raw().update_local_participant(std::move(value), std::move(completion), options);
    });
  }

  ErrorCode set_subscription_permissions(rtc::SubscriptionPermissions value,
                                         rtc::ControlOptions options = {}) {
    return observer_->template wait<ErrorCode>("permissions", [&](auto completion) {
      return room_raw().set_subscription_permissions(std::move(value), std::move(completion),
                                                     options);
    });
  }

  Result<rtc::engine::MediaPublication> publish_media(rtc::engine::MediaPublishSpec spec) {
    guard_media_callbacks(spec);
    const auto id = spec.track.id;
    return observer_->template wait<Result<rtc::engine::MediaPublication>>(
        "publish:" + id, [&](auto completion) {
          return engine_->publish_media(std::move(spec), std::move(completion));
        });
  }

  Result<rtc::engine::MediaPublication> publish_video(rtc::engine::MediaPublishSpec spec) {
    guard_media_callbacks(spec);
    const auto id = spec.track.id;
    return observer_->template wait<Result<rtc::engine::MediaPublication>>(
        "publish:" + id, [&](auto completion) {
          return engine_->publish_video(std::move(spec), std::move(completion));
        });
  }

  Result<rtc::engine::MediaPublication> publish_audio(rtc::engine::MediaPublishSpec spec) {
    guard_media_callbacks(spec);
    const auto id = spec.track.id;
    return observer_->template wait<Result<rtc::engine::MediaPublication>>(
        "publish:" + id, [&](auto completion) {
          return engine_->publish_audio(std::move(spec), std::move(completion));
        });
  }

  Result<rtc::engine::MediaSubscription> subscribe_media(rtc::engine::MediaSubscribeSpec spec) {
    guard_media_callbacks(spec.output);
    const auto id = spec.track.id;
    return observer_->template wait<Result<rtc::engine::MediaSubscription>>(
        "subscribe:" + id, [&](auto completion) {
          return engine_->subscribe_media(std::move(spec), std::move(completion));
        });
  }

  ErrorCode enable_media(rtc::AudioOutputOptions options = {}) {
    return engine_->enable_media(std::move(options));
  }

  bool media_enabled() const {
    return engine_->media_enabled();
  }

  auto create_shared_input(rtc::TrackDescriptor format, rtc::engine::MediaInput input) {
    const auto id = format.id;
    return observer_->template wait<Result<std::shared_ptr<rtc::engine::SharedMediaInput>>>(
        "input:" + id, [&](auto completion) {
          return engine_->create_shared_input(std::move(format), std::move(input),
                                              std::move(completion));
        });
  }

  auto snapshot() const {
    return room_raw().snapshot();
  }

  auto services() const {
    return room_raw().services();
  }

  class RoomBridge {
   public:
    explicit RoomBridge(BlockingEngine& owner) : owner_(&owner) {}

    auto snapshot() const {
      return owner_->snapshot();
    }

    auto services() const {
      return owner_->services();
    }

    ErrorCode update_local_participant(rtc::ParticipantUpdate value,
                                       rtc::ControlOptions options = {}) const {
      return owner_->update_local_participant(std::move(value), options);
    }

    ErrorCode set_subscription_permissions(rtc::SubscriptionPermissions value,
                                           rtc::ControlOptions options = {}) const {
      return owner_->set_subscription_permissions(std::move(value), options);
    }

   private:
    BlockingEngine* owner_;
  };

  RoomBridge room() {
    return RoomBridge(*this);
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
      return owner_.observer_->template wait<Result<rtc::TrackId>>(
          "publish:" + id, [&](auto completion) {
            return graph_.publish(std::move(d), std::forward<Args>(args)..., std::move(completion));
          });
    }

    template <class... Args>
    Result<rtc::TrackId> subscribe(rtc::TrackDescriptor d, Args&&... args) {
      const auto id = d.id;
      return owner_.observer_->template wait<Result<rtc::TrackId>>(
          "subscribe:" + id, [&](auto completion) {
            return graph_.subscribe(std::move(d), std::forward<Args>(args)...,
                                    std::move(completion));
          });
    }

    template <class Factory>
    Result<rtc::TrackId> publish_graph(rtc::TrackDescriptor d, Factory factory) {
      const auto id = d.id;
      return owner_.observer_->template wait<Result<rtc::TrackId>>(
          "publish:" + id, [&](auto completion) {
            return graph_.publish_graph(
                std::move(d),
                [factory = std::move(factory)](auto node) mutable {
                  BridgeCallbackScope scope;
                  auto spec = factory(std::move(node));
                  if (spec.activate) {
                    auto hook = std::move(spec.activate);
                    spec.activate = [hook = std::move(hook)] {
                      BridgeCallbackScope scope;
                      hook();
                    };
                  }
                  if (spec.quiesce) {
                    auto hook = std::move(spec.quiesce);
                    spec.quiesce = [hook = std::move(hook)] {
                      BridgeCallbackScope scope;
                      hook();
                    };
                  }
                  return spec;
                },
                std::move(completion));
          });
    }

    template <class Factory>
    Result<rtc::TrackId> subscribe_graph(rtc::TrackDescriptor d, Factory factory) {
      const auto id = d.id;
      return owner_.observer_->template wait<Result<rtc::TrackId>>(
          "subscribe:" + id, [&](auto completion) {
            return graph_.subscribe_graph(
                std::move(d),
                [factory = std::move(factory)](auto node) mutable {
                  BridgeCallbackScope scope;
                  auto spec = factory(std::move(node));
                  if (spec.activate) {
                    auto hook = std::move(spec.activate);
                    spec.activate = [hook = std::move(hook)] {
                      BridgeCallbackScope scope;
                      hook();
                    };
                  }
                  if (spec.quiesce) {
                    auto hook = std::move(spec.quiesce);
                    spec.quiesce = [hook = std::move(hook)] {
                      BridgeCallbackScope scope;
                      hook();
                    };
                  }
                  return spec;
                },
                std::move(completion));
          });
    }

   private:
    BlockingEngine& owner_;
    Graph graph_;
  };

  auto graph() {
    return GraphBridge(*this, engine_->graph());
  }

 private:
  decltype(auto) room_raw() const {
    if constexpr (std::is_same_v<Engine, rtc::RoomEngine>) {
      return *engine_;
    } else {
      return engine_->room();
    }
  }

  std::shared_ptr<BlockingObservers> observer_ = std::make_shared<BlockingObservers>();
  std::unique_ptr<Engine> engine_;
  bool closed_ = false;
};

using BlockingRoom = BlockingEngine<rtc::RoomEngine>;
using BlockingRtc = BlockingEngine<rtc::engine::RtcEngine>;

inline Result<std::unique_ptr<BlockingRoom>> create_blocking_room() {
  auto result = rtc::create_livekit_room();
  if (!result) {
    return Result<std::unique_ptr<BlockingRoom>>::failure(result.failure_record());
  }
  return Result<std::unique_ptr<BlockingRoom>>::success(
      std::make_unique<BlockingRoom>(*std::move(result)));
}

inline Result<std::unique_ptr<BlockingRtc>> create_blocking_engine() {
  auto result = rtc::engine::create_livekit_engine();
  if (!result) {
    return Result<std::unique_ptr<BlockingRtc>>::failure(result.failure_record());
  }
  return Result<std::unique_ptr<BlockingRtc>>::success(
      std::make_unique<BlockingRtc>(*std::move(result)));
}

inline Result<std::unique_ptr<BlockingRtc>> create_blocking_engine(
    rtc::AudioOutputOptions options) {
  auto result = rtc::engine::create_livekit_engine(std::move(options));
  if (!result) {
    return Result<std::unique_ptr<BlockingRtc>>::failure(result.failure_record());
  }
  return Result<std::unique_ptr<BlockingRtc>>::success(
      std::make_unique<BlockingRtc>(*std::move(result)));
}
}  // namespace livekit::sdk::app_support
