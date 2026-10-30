#pragma once
#include "livekit/rtc/stream_pipeline.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace livekit::sdk::rtc::engine {
// Runs per-track graphs. No room, transport commands or device ownership.
// Lifecycle calls require the owner's control sequence. Reads/writes use only
// short registry locks; workers never stop or join a graph.
class TrackGraphRegistry {
 public:
  TrackGraphRegistry() = default;
  ~TrackGraphRegistry();
  TrackGraphRegistry(const TrackGraphRegistry&) = delete;
  TrackGraphRegistry& operator=(const TrackGraphRegistry&) = delete;

  std::shared_ptr<PublishPipeline> prepare(std::shared_ptr<nodes::RtcPublishNode>, TrackGraphSpec);
  std::shared_ptr<SubscribePipeline> prepare(std::shared_ptr<nodes::RtcSubscribeNode>,
                                             TrackGraphSpec);
  void install(const std::shared_ptr<PublishPipeline>&);
  void install(const std::shared_ptr<SubscribePipeline>&);

  // Returned graphs are stopped but retained until the coordinator has awaited
  // native endpoint detachment. Dropping these leases releases graph resources.
  struct RetiredGraphs {
    std::vector<std::shared_ptr<PublishPipeline>> publications;
    std::vector<std::shared_ptr<SubscribePipeline>> subscriptions;
  };

  RetiredGraphs take(const TrackId&, bool publication);
  RetiredGraphs take_all();
  RetiredGraphs take_failed();

  WriteStatus write(const TrackId&, EncodedFrame);
  TrackSnapshot publication(const TrackId&) const;
  SenderFeedback feedback(const TrackId&) const;

  struct GraphFailure {
    TrackId id;
    bool publication;
    std::string message;
    std::shared_ptr<const FailureInfo> failure;
  };

  void set_failure_handler(std::function<void(GraphFailure)>);
  // Schedules owner-side reclamation; never stops a graph on a reporting worker.
  void set_reap_handler(std::function<void()>);

 private:
  static void stop(const RetiredGraphs&);
  template <class Endpoint>
  GraphStopped observer(const std::shared_ptr<Endpoint>&);

  struct Reports {
    std::mutex mutex;
    std::map<std::shared_ptr<PublishedTrack>, std::exception_ptr> publications;
    std::map<std::shared_ptr<SubscribedTrack>, std::exception_ptr> subscriptions;
    std::function<void(GraphFailure)> handler;
    std::function<void()> reap;
  };

  std::shared_ptr<Reports> reports_ = std::make_shared<Reports>();
  mutable std::mutex mutex_;
  std::map<TrackId, std::shared_ptr<PublishPipeline>> publications_;
  std::map<TrackId, std::shared_ptr<SubscribePipeline>> subscriptions_;
};
}  // namespace livekit::sdk::rtc::engine
