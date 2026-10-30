#include "engines/rtc/track_graph_registry.hpp"
#include "backends/rtc/control_context.hpp"

namespace livekit::sdk::rtc::engine {
TrackGraphRegistry::~TrackGraphRegistry() {
  (void)take_all();
}

template <class Endpoint>
GraphStopped TrackGraphRegistry::observer(const std::shared_ptr<Endpoint>& endpoint) {
  return [reports = reports_, weak = std::weak_ptr<Endpoint>(endpoint)](auto failure) {
    auto endpoint = weak.lock();
    if (!endpoint) {
      return;
    }
    // Reporting never waits for control or stops a graph on its own worker.
    if (failure) {
      endpoint->report_failure(failure);
    }
    std::function<void(GraphFailure)> handler;
    std::function<void()> reap;
    {
      std::lock_guard lock(reports->mutex);
      if constexpr (std::is_same_v<Endpoint, PublishedTrack>) {
        reports->publications[endpoint] = failure;
      } else {
        reports->subscriptions[endpoint] = failure;
      }
      handler = reports->handler;
      reap = reports->reap;
    }
    if (reap) {
      reap();
    }
    if (failure && handler) {
      auto info = describe_failure(failure);
      handler({endpoint->descriptor().id, std::is_same_v<Endpoint, PublishedTrack>, info->message,
               info});
    }
  };
}

void TrackGraphRegistry::set_failure_handler(std::function<void(GraphFailure)> handler) {
  std::lock_guard lock(reports_->mutex);
  reports_->handler = std::move(handler);
}

void TrackGraphRegistry::set_reap_handler(std::function<void()> handler) {
  std::lock_guard lock(reports_->mutex);
  reports_->reap = std::move(handler);
}

TrackGraphRegistry::RetiredGraphs TrackGraphRegistry::take_failed() {
  RetiredGraphs retired;
  decltype(Reports::publications) publications;
  decltype(Reports::subscriptions) subscriptions;
  {
    std::lock_guard lock(reports_->mutex);
    publications.swap(reports_->publications);
    subscriptions.swap(reports_->subscriptions);
  }
  for (const auto& [endpoint, failure] : publications) {
    std::shared_ptr<PublishPipeline> graph;
    {
      std::lock_guard lock(mutex_);
      auto it = publications_.find(endpoint->descriptor().id);
      if (it != publications_.end() && it->second->node().track() == endpoint) {
        graph = std::move(it->second);
        publications_.erase(it);
      }
    }
    if (graph) {
      if (failure) {
        graph->notify_failure(failure);
      }
      retired.publications.push_back(std::move(graph));
    }
  }
  for (const auto& [endpoint, failure] : subscriptions) {
    std::shared_ptr<SubscribePipeline> graph;
    {
      std::lock_guard lock(mutex_);
      auto it = subscriptions_.find(endpoint->descriptor().id);
      if (it != subscriptions_.end() && it->second->node().track() == endpoint) {
        graph = std::move(it->second);
        subscriptions_.erase(it);
      }
    }
    if (graph) {
      if (failure) {
        graph->notify_failure(failure);
      }
      retired.subscriptions.push_back(std::move(graph));
    }
  }
  stop(retired);
  return retired;
}

std::shared_ptr<PublishPipeline> TrackGraphRegistry::prepare(
    std::shared_ptr<nodes::RtcPublishNode> node, TrackGraphSpec spec) {
  auto report = observer(node->track());
  return std::shared_ptr<PublishPipeline>(
      new PublishPipeline(std::move(node), std::move(spec), std::move(report)));
}

std::shared_ptr<SubscribePipeline> TrackGraphRegistry::prepare(
    std::shared_ptr<nodes::RtcSubscribeNode> node, TrackGraphSpec spec) {
  auto report = observer(node->track());
  return std::shared_ptr<SubscribePipeline>(
      new SubscribePipeline(std::move(node), std::move(spec), std::move(report)));
}

void TrackGraphRegistry::install(const std::shared_ptr<PublishPipeline>& graph) {
  graph->start_locked();
  std::lock_guard lock(mutex_);
  if (!publications_.emplace(graph->node().track()->descriptor().id, graph).second) {
    throw std::logic_error("RTC publication graph already exists");
  }
}

void TrackGraphRegistry::install(const std::shared_ptr<SubscribePipeline>& graph) {
  graph->start_locked();
  std::lock_guard lock(mutex_);
  if (!subscriptions_.emplace(graph->node().track()->descriptor().id, graph).second) {
    throw std::logic_error("RTC subscription graph already exists");
  }
}

void TrackGraphRegistry::stop(const RetiredGraphs& retired) {
  for (const auto& graph : retired.publications) {
    (void)graph->close_locked();
  }
  for (const auto& graph : retired.subscriptions) {
    (void)graph->close_locked();
  }
}

TrackGraphRegistry::RetiredGraphs TrackGraphRegistry::take(const TrackId& id, bool publication) {
  RetiredGraphs retired;
  {
    std::lock_guard lock(mutex_);
    if (publication) {
      auto it = publications_.find(id);
      if (it != publications_.end()) {
        retired.publications.push_back(std::move(it->second));
        publications_.erase(it);
      }
    } else {
      auto it = subscriptions_.find(id);
      if (it != subscriptions_.end()) {
        retired.subscriptions.push_back(std::move(it->second));
        subscriptions_.erase(it);
      }
    }
  }
  stop(retired);
  return retired;
}

TrackGraphRegistry::RetiredGraphs TrackGraphRegistry::take_all() {
  RetiredGraphs retired;
  {
    std::lock_guard lock(mutex_);
    retired.publications.reserve(publications_.size());
    retired.subscriptions.reserve(subscriptions_.size());
    for (auto& [id, graph] : publications_) {
      retired.publications.push_back(std::move(graph));
    }
    for (auto& [id, graph] : subscriptions_) {
      retired.subscriptions.push_back(std::move(graph));
    }
    publications_.clear();
    subscriptions_.clear();
  }
  stop(retired);
  {
    std::lock_guard lock(reports_->mutex);
    reports_->publications.clear();
    reports_->subscriptions.clear();
  }
  return retired;
}

WriteStatus TrackGraphRegistry::write(const TrackId& id, EncodedFrame frame) {
  std::shared_ptr<PublishPipeline> graph;
  {
    std::lock_guard lock(mutex_);
    auto it = publications_.find(id);
    if (it == publications_.end())
      return WriteStatus::closed;
    graph = it->second;
  }
  return graph->write(std::move(frame));
}

TrackSnapshot TrackGraphRegistry::publication(const TrackId& id) const {
  std::lock_guard lock(mutex_);
  auto it = publications_.find(id);
  if (it == publications_.end()) {
    return {};
  }
  return it->second->node().track()->snapshot();
}

SenderFeedback TrackGraphRegistry::feedback(const TrackId& id) const {
  std::lock_guard lock(mutex_);
  auto it = publications_.find(id);
  if (it == publications_.end()) {
    return {};
  }
  return it->second->node().track()->feedback();
}

}  // namespace livekit::sdk::rtc::engine
