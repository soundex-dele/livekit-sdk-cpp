#pragma once
// Non-installed bridge for existing blocking CLI/test supervisors. The SDK API
// remains observer-only; new applications should implement its observers directly.
#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/livekit.hpp"
#include "rtc_result.hpp"
#include <any>
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

struct BlockingObservers : rtc::RoomObserver,
                           rtc::PublicationObserver,
                           rtc::SubscriptionObserver,
                           rtc::engine::PublicationObserver,
                           rtc::engine::SubscriptionObserver {
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::string, std::any> results;
  std::function<void(const rtc::RoomEvent&)> event;
  std::function<void(const rtc::engine::GraphFailure&)> failure;
  size_t invoking = 0;

  template <class T>
  void complete(const std::string& key, T result) {
    std::lock_guard lock(mutex);
    if (auto it = results.find(key); it != results.end()) {
      it->second = std::move(result);
    }
    changed.notify_all();
  }

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
  T wait(const std::string& key, Submit submit) {
    if (in_blocking_bridge_callback) {
      return rejected<T>(ErrorCode::invalid_thread);
    }
    {
      std::lock_guard lock(mutex);
      if (!results.emplace(key, std::any{}).second) {
        return rejected<T>(ErrorCode::busy);
      }
    }
    auto code = submit();
    std::unique_lock lock(mutex);
    if (code != ErrorCode::ok) {
      results.erase(key);
      return rejected<T>(code);
    }
    changed.wait(lock, [&] { return results.at(key).has_value(); });
    auto result = std::any_cast<T>(std::move(results.at(key)));
    results.erase(key);
    return result;
  }

  ErrorCode set_handlers(std::function<void(const rtc::RoomEvent&)> e,
                         std::function<void(const rtc::engine::GraphFailure&)> f) {
    if (in_blocking_bridge_callback) {
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

  void on_join_completed(const rtc::OperationResult& r) override {
    complete("join", r.code);
  }

  void on_leave_completed(const rtc::OperationResult& r) override {
    complete("leave", r.code);
  }

  void on_close_completed(const rtc::OperationResult& r) override {
    complete("close", r.code);
  }

  void on_participant_update_completed(const rtc::OperationResult& r) override {
    complete("participant", r.code);
  }

  void on_subscription_permissions_completed(const rtc::OperationResult& r) override {
    complete("permissions", r.code);
  }

  void on_unpublish_completed(const rtc::TrackId& id, const rtc::OperationResult& r) override {
    complete("unpublish:" + id, r.code);
  }

  void on_unsubscribe_completed(const rtc::TrackId& id, const rtc::OperationResult& r) override {
    complete("unsubscribe:" + id, r.code);
  }

  void on_muted_completed(const rtc::TrackId& id, bool, const rtc::OperationResult& r) override {
    complete("mute:" + id, r.code);
  }

  void on_receive_demand_completed(const rtc::TrackId& id, rtc::ReceiveDemand,
                                   const rtc::OperationResult& r) override {
    complete("demand:" + id, r.code);
  }

  void on_publish_completed(const rtc::TrackId& id,
                            Result<std::shared_ptr<rtc::PublishedTrack>> r) override {
    complete("publish:" + id, std::move(r));
  }

  void on_subscribe_completed(const rtc::TrackId& id,
                              Result<std::shared_ptr<rtc::SubscribedTrack>> r) override {
    complete("subscribe:" + id, std::move(r));
  }

  void on_publish_completed(const rtc::TrackId& id, Result<rtc::TrackId> r) override {
    complete("publish:" + id, std::move(r));
  }

  void on_subscribe_completed(const rtc::TrackId& id, Result<rtc::TrackId> r) override {
    complete("subscribe:" + id, std::move(r));
  }

  void on_shared_input_created(const rtc::TrackId& id,
                               Result<std::shared_ptr<rtc::engine::SharedMediaInput>> r) override {
    complete("input:" + id, std::move(r));
  }

  void on_media_publish_completed(const rtc::TrackId& id,
                                  Result<rtc::engine::MediaPublication> r) override {
    complete("publish:" + id, std::move(r));
  }

  void on_media_subscribe_completed(const rtc::TrackId& id,
                                    Result<rtc::engine::MediaSubscription> r) override {
    complete("subscribe:" + id, std::move(r));
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
    return observer_->template wait<ErrorCode>("join",
                                               [&] { return engine_->join(std::move(options)); });
  }

  ErrorCode leave() {
    return observer_->template wait<ErrorCode>("leave", [&] { return engine_->leave(); });
  }

  ErrorCode close() {
    if (in_blocking_bridge_callback) {
      return ErrorCode::invalid_thread;
    }
    if (closed_) {
      return ErrorCode::ok;
    }
    auto code = observer_->template wait<ErrorCode>("close", [&] { return engine_->close(); });
    if (code == ErrorCode::ok) {
      closed_ = true;
    }
    return code;
  }

  auto publish(rtc::TrackDescriptor descriptor) {
    using Value = std::conditional_t<std::is_same_v<Engine, rtc::RoomEngine>,
                                     std::shared_ptr<rtc::PublishedTrack>, rtc::TrackId>;
    const auto id = descriptor.id;
    return observer_->template wait<Result<Value>>(
        "publish:" + id, [&] { return engine_->publish(std::move(descriptor)); });
  }

  auto subscribe(rtc::TrackDescriptor descriptor) {
    const auto id = descriptor.id;
    return observer_->template wait<Result<std::shared_ptr<rtc::SubscribedTrack>>>(
        "subscribe:" + id, [&] { return engine_->subscribe(std::move(descriptor)); });
  }

  ErrorCode unpublish(const rtc::TrackId& id) {
    return observer_->template wait<ErrorCode>("unpublish:" + id,
                                               [&] { return engine_->unpublish(id); });
  }

  ErrorCode unsubscribe(const rtc::TrackId& id) {
    return observer_->template wait<ErrorCode>("unsubscribe:" + id,
                                               [&] { return engine_->unsubscribe(id); });
  }

  ErrorCode set_muted(const rtc::TrackId& id, bool muted, rtc::ControlOptions options = {}) {
    return observer_->template wait<ErrorCode>(
        "mute:" + id, [&] { return engine_->set_muted(id, muted, options); });
  }

  ErrorCode set_receive_demand(const rtc::TrackId& id, rtc::ReceiveDemand demand,
                               rtc::ControlOptions options = {}) {
    return observer_->template wait<ErrorCode>(
        "demand:" + id, [&] { return engine_->set_receive_demand(id, demand, options); });
  }

  ErrorCode update_local_participant(rtc::ParticipantUpdate value,
                                     rtc::ControlOptions options = {}) {
    return observer_->template wait<ErrorCode>("participant", [&] {
      return room_raw().update_local_participant(std::move(value), options);
    });
  }

  ErrorCode set_subscription_permissions(rtc::SubscriptionPermissions value,
                                         rtc::ControlOptions options = {}) {
    return observer_->template wait<ErrorCode>("permissions", [&] {
      return room_raw().set_subscription_permissions(std::move(value), options);
    });
  }

  Result<rtc::engine::MediaPublication> publish_media(rtc::engine::MediaPublishSpec spec) {
    const auto id = spec.track.id;
    return observer_->template wait<Result<rtc::engine::MediaPublication>>(
        "publish:" + id, [&] { return engine_->publish_media(std::move(spec)); });
  }

  Result<rtc::engine::MediaPublication> publish_video(rtc::engine::MediaPublishSpec spec) {
    const auto id = spec.track.id;
    return observer_->template wait<Result<rtc::engine::MediaPublication>>(
        "publish:" + id, [&] { return engine_->publish_video(std::move(spec)); });
  }

  Result<rtc::engine::MediaPublication> publish_audio(rtc::engine::MediaPublishSpec spec) {
    const auto id = spec.track.id;
    return observer_->template wait<Result<rtc::engine::MediaPublication>>(
        "publish:" + id, [&] { return engine_->publish_audio(std::move(spec)); });
  }

  Result<rtc::engine::MediaSubscription> subscribe_media(rtc::engine::MediaSubscribeSpec spec) {
    const auto id = spec.track.id;
    return observer_->template wait<Result<rtc::engine::MediaSubscription>>(
        "subscribe:" + id, [&] { return engine_->subscribe_media(std::move(spec)); });
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
        "input:" + id,
        [&] { return engine_->create_shared_input(std::move(format), std::move(input)); });
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
      return owner_.observer_->template wait<Result<rtc::TrackId>>("publish:" + id, [&] {
        return graph_.publish(std::move(d), std::forward<Args>(args)...);
      });
    }

    template <class... Args>
    Result<rtc::TrackId> subscribe(rtc::TrackDescriptor d, Args&&... args) {
      const auto id = d.id;
      return owner_.observer_->template wait<Result<rtc::TrackId>>("subscribe:" + id, [&] {
        return graph_.subscribe(std::move(d), std::forward<Args>(args)...);
      });
    }

    template <class Factory>
    Result<rtc::TrackId> publish_graph(rtc::TrackDescriptor d, Factory factory) {
      const auto id = d.id;
      return owner_.observer_->template wait<Result<rtc::TrackId>>("publish:" + id, [&] {
        return graph_.publish_graph(std::move(d),
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
                                    });
      });
    }

    template <class Factory>
    Result<rtc::TrackId> subscribe_graph(rtc::TrackDescriptor d, Factory factory) {
      const auto id = d.id;
      return owner_.observer_->template wait<Result<rtc::TrackId>>("subscribe:" + id, [&] {
        return graph_.subscribe_graph(std::move(d),
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
                                      });
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
