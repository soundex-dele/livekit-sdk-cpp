#include "engines/rtc/pipeline_engine.hpp"
#include "backends/rtc/control_context.hpp"

namespace livekit::sdk::rtc::engine {
RtcPipelineEngine::RtcPipelineEngine(RoomEngine& room) : room_(&room) {}

RtcPipelineEngine::~RtcPipelineEngine() {
  (void)clear();
}

template <class Endpoint>
GraphStopped RtcPipelineEngine::observer(const std::shared_ptr<Endpoint>& endpoint) {
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
    {
      std::lock_guard lock(reports->mutex);
      if constexpr (std::is_same_v<Endpoint, PublishedTrack>) {
        reports->publications[endpoint] = failure;
      } else {
        reports->subscriptions[endpoint] = failure;
      }
      handler = reports->handler;
    }
    if (failure && handler) {
      auto info = describe_failure(failure);
      handler({endpoint->descriptor().id, std::is_same_v<Endpoint, PublishedTrack>, info->message,
               info});
    }
  };
}

void RtcPipelineEngine::set_failure_handler(std::function<void(GraphFailure)> handler) {
  std::lock_guard lock(reports_->mutex);
  reports_->handler = std::move(handler);
}

void RtcPipelineEngine::reap_failed() {
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
      (void)graph->close_locked();
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
      (void)graph->close_locked();
    }
  }
}

Result<TrackId> RtcPipelineEngine::publish_graph(TrackDescriptor descriptor,
                                                 PublishGraphFactory factory,
                                                 FailureCleanup cleanup) {
  return detail::control_boundary([&]() -> Result<TrackId> {
    if (!factory) {
      throw std::invalid_argument("RTC engine requires a graph factory");
    }
    auto ready = room_->publish_locked(descriptor);
    if (!ready) {
      if (cleanup) {
        cleanup(std::make_exception_ptr(SDKError(ready.failure_record())));
      }
      return Result<TrackId>::failure(ready.failure_record());
    }
    auto endpoint = *ready;
    try {
      auto graph =
          std::make_shared<PublishPipeline>(endpoint, std::move(factory), observer(endpoint));
      graph->start_locked();
      {
        std::lock_guard lock(mutex_);
        if (!publications_.emplace(descriptor.id, graph).second) {
          throw std::logic_error("RTC track graph already exists");
        }
      }
      return Result<TrackId>::success(descriptor.id);
    } catch (...) {
      auto error = normalized_failure(
          std::current_exception(), {"create_graph", descriptor.id, {}, endpoint->snapshot().epoch},
          ErrorDomain::media, ErrorCode::processing_failed);
      endpoint->report_failure(error);
      (void)endpoint->close_locked();
      if (cleanup) {
        cleanup(error);
      }
      return Result<TrackId>::failure(describe_failure(error));
    }
  });
}

Result<TrackId> RtcPipelineEngine::subscribe_graph(TrackDescriptor descriptor,
                                                   SubscribeGraphFactory factory,
                                                   FailureCleanup cleanup) {
  return detail::control_boundary([&]() -> Result<TrackId> {
    if (!factory) {
      throw std::invalid_argument("RTC engine requires a graph factory");
    }
    auto ready = room_->subscribe_locked(descriptor);
    if (!ready) {
      if (cleanup) {
        cleanup(std::make_exception_ptr(SDKError(ready.failure_record())));
      }
      return Result<TrackId>::failure(ready.failure_record());
    }
    auto endpoint = *ready;
    try {
      auto graph =
          std::make_shared<SubscribePipeline>(endpoint, std::move(factory), observer(endpoint));
      graph->start_locked();
      {
        std::lock_guard lock(mutex_);
        if (!subscriptions_.emplace(descriptor.id, graph).second) {
          throw std::logic_error("RTC track graph already exists");
        }
      }
      return Result<TrackId>::success(descriptor.id);
    } catch (...) {
      auto error = normalized_failure(
          std::current_exception(), {"create_graph", descriptor.id, {}, endpoint->snapshot().epoch},
          ErrorDomain::media, ErrorCode::processing_failed);
      endpoint->report_failure(error);
      (void)endpoint->close_locked();
      if (cleanup) {
        cleanup(error);
      }
      return Result<TrackId>::failure(describe_failure(error));
    }
  });
}

Result<TrackId> RtcPipelineEngine::publish(
    TrackDescriptor descriptor, std::shared_ptr<graphflow::core::GraphNode> feedback_sink) {
  return publish_graph(std::move(descriptor), [feedback_sink](auto sink) {
    TrackGraphSpec spec;
    if (feedback_sink) {
      spec.nodes.push_back(feedback_sink);
      spec.connections.push_back(
          TrackGraphSpec::connect<nodes::SenderFeedbackEvent>(sink, feedback_sink));
    }
    return spec;
  });
}

Result<TrackId> RtcPipelineEngine::subscribe(TrackDescriptor descriptor,
                                             std::shared_ptr<graphflow::core::GraphNode> consumer) {
  if (!consumer)
    throw std::invalid_argument("RTC engine requires a subscription consumer node");
  return subscribe_graph(std::move(descriptor), [consumer](auto source) {
    TrackGraphSpec spec;
    spec.nodes.push_back(consumer);
    spec.connections.push_back(TrackGraphSpec::connect<nodes::EncodedFrameEvent>(source, consumer));
    return spec;
  });
}

Result<void> RtcPipelineEngine::remove_graph(const TrackId& id, bool publication) {
  std::shared_ptr<PublishPipeline> publish;
  std::shared_ptr<SubscribePipeline> subscribe;
  {
    std::lock_guard lock(mutex_);
    if (publication) {
      auto it = publications_.find(id);
      if (it != publications_.end()) {
        publish = std::move(it->second);
        publications_.erase(it);
      }
    } else {
      auto it = subscriptions_.find(id);
      if (it != subscriptions_.end()) {
        subscribe = std::move(it->second);
        subscriptions_.erase(it);
      }
    }
  }
  if (publish) {
    return publish->close_locked();
  }
  if (subscribe) {
    return subscribe->close_locked();
  }
  return Result<void>::success();
}

WriteStatus RtcPipelineEngine::write(const TrackId& id, EncodedFrame frame) {
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

TrackSnapshot RtcPipelineEngine::publication(const TrackId& id) const {
  std::lock_guard lock(mutex_);
  auto it = publications_.find(id);
  if (it == publications_.end())
    throw std::out_of_range("Unknown RTC engine publication");
  return it->second->node().track()->snapshot();
}

SenderFeedback RtcPipelineEngine::feedback(const TrackId& id) const {
  std::lock_guard lock(mutex_);
  auto it = publications_.find(id);
  if (it == publications_.end())
    throw std::out_of_range("Unknown RTC engine publication");
  return it->second->node().track()->feedback();
}

Result<void> RtcPipelineEngine::unpublish(const TrackId& id) {
  auto graph = remove_graph(id, true);
  auto endpoint = room_->unpublish_locked(id);
  return graph ? endpoint : graph;
}

Result<void> RtcPipelineEngine::unsubscribe(const TrackId& id) {
  auto graph = remove_graph(id, false);
  auto endpoint = room_->unsubscribe_locked(id);
  return graph ? endpoint : graph;
}

Result<void> RtcPipelineEngine::clear() {
  std::map<TrackId, std::shared_ptr<PublishPipeline>> publications;
  std::map<TrackId, std::shared_ptr<SubscribePipeline>> subscriptions;
  {
    std::lock_guard lock(mutex_);
    publications.swap(publications_);
    subscriptions.swap(subscriptions_);
  }
  auto result = Result<void>::success();
  for (auto& [id, graph] : publications) {
    auto closed = graph->close_locked();
    if (!closed && result) {
      result = std::move(closed);
    }
  }
  for (auto& [id, graph] : subscriptions) {
    auto closed = graph->close_locked();
    if (!closed && result) {
      result = std::move(closed);
    }
  }
  reap_failed();
  return result;
}
}  // namespace livekit::sdk::rtc::engine
