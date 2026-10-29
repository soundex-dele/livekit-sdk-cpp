#pragma once
#include "livekit/rtc/stream_pipeline.hpp"
#include <functional>
#include <deque>
#include <thread>

namespace livekit::sdk::rtc::engine {

// Scene-engine building block. One RoomBackend is shared by all track graphs.
// The scene controller calls poll() to complete graph creation and fairly drive
// receive/feedback nodes. No per-track worker and no user media work runs on the
// backend's signaling/transport thread. Control methods are owner-thread only;
// write() may be called by media producers. Consumers must not reenter graph
// lifecycle methods from inside synchronous GraphEngine delivery.
class RtcEngine {
 public:
  explicit RtcEngine(std::unique_ptr<detail::RoomTransport>);
  explicit RtcEngine(std::unique_ptr<RoomBackend>);
  ~RtcEngine();
  RtcEngine(const RtcEngine&) = delete;
  RtcEngine& operator=(const RtcEngine&) = delete;

  std::future<void> join(RoomOptions);
  std::future<TrackId> publish(TrackDescriptor,
                               std::shared_ptr<graphflow::core::GraphNode> feedback_sink = {});
  std::future<TrackId> subscribe(TrackDescriptor,
                                 std::shared_ptr<graphflow::core::GraphNode> consumer);
  // Factories run on poll() only after endpoint confirmation. They create
  // capture/codec/recording/playout nodes in the endpoint's own GraphEngine.
  std::future<TrackId> publish_graph(TrackDescriptor, PublishGraphFactory);
  std::future<TrackId> subscribe_graph(TrackDescriptor, SubscribeGraphFactory);
  std::future<void> unpublish(const TrackId&);
  std::future<void> unsubscribe(const TrackId&);
  std::future<void> set_muted(const TrackId&, bool muted, ControlOptions = {});
  std::future<void> set_receive_demand(const TrackId&, ReceiveDemand, ControlOptions = {});
  WriteStatus write(const TrackId&, EncodedFrame);
  TrackSnapshot publication(const TrackId&) const;
  SenderFeedback feedback(const TrackId&) const;
  void poll(size_t receive_frames_per_track = 4);

  struct GraphFailure {
    TrackId id;
    bool publication;
    std::string message;
  };

  std::optional<GraphFailure> next_failure();
  std::future<void> close();

  RoomBackend& room() {
    return *room_;
  }

 private:
  void require_owner() const;
  void remove_graph(const TrackId&, bool publication);
  void cancel_pending(const TrackId&, bool publication);
  void failed_graph(const TrackId&, bool publication, std::string message);

  struct Pending {
    TrackId id;
    bool publication;
    std::function<bool()> poll;
    std::function<void(std::exception_ptr)> cancel;
  };

  std::unique_ptr<RoomBackend> room_;
  const std::thread::id owner_ = std::this_thread::get_id();
  mutable std::mutex mutex_;
  std::map<TrackId, std::shared_ptr<PublishPipeline>> publications_;
  std::map<TrackId, std::shared_ptr<SubscribePipeline>> subscriptions_;
  std::vector<Pending> pending_;
  std::deque<GraphFailure> failures_;
  bool closed_ = false;
};

// Available when the native LiveKit backend is built.
std::unique_ptr<RtcEngine> create_livekit_engine();
}  // namespace livekit::sdk::rtc::engine
