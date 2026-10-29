#include "engines/rtc/pipeline_engine.hpp"
#include "backends/rtc/room_transport.hpp"
#include "livekit/error.hpp"

namespace livekit::sdk::rtc::engine {
namespace {
std::exception_ptr closed_error() {
  return std::make_exception_ptr(
      SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed, "RTC engine closed"));
}
}  // namespace

RtcPipelineEngine::RtcPipelineEngine(RoomEngine& room, Dispatch dispatch)
    : room_(&room), dispatch_(std::move(dispatch)) {}

RtcPipelineEngine::~RtcPipelineEngine() {
  lifetime_.reset();
  // Destruction, like all graph lifecycle operations, must follow producer
  // quiescence. RoomEngine destruction supplies the final native barrier.
  for (auto& pending : pending_) {
    pending->settled = true;
    pending->cancel(closed_error());
  }
  pending_.clear();
  std::map<TrackId, std::shared_ptr<PublishPipeline>> publications;
  std::map<TrackId, std::shared_ptr<SubscribePipeline>> subscriptions;
  {
    std::lock_guard lock(mutex_);
    publications.swap(publications_);
    subscriptions.swap(subscriptions_);
  }
  // Codec/source workers may be finishing a write(). Never hold the engine
  // registry mutex while their graph teardown waits for them to quiesce.
  publications.clear();
  subscriptions.clear();
}

void RtcPipelineEngine::require_owner() const {
  if (std::this_thread::get_id() != owner_)
    throw std::logic_error("RTC graph control requires its owner thread");
}

template <class Endpoint>
GraphStopped RtcPipelineEngine::observer(const std::shared_ptr<Endpoint>& endpoint) {
  return [this, lifetime = std::weak_ptr<int>(lifetime_), weak = std::weak_ptr<Endpoint>(endpoint),
          dispatch = dispatch_](auto failure) {
    // Only the dispatched closure may access the owner; destruction invalidates
    // the token on this same sequence. Endpoint identity fences reused track IDs.
    dispatch([this, lifetime, weak, failure] {
      if (lifetime.expired()) {
        return;
      }
      if (auto endpoint = weak.lock()) {
        stopped_graph(endpoint, failure);
      }
    });
  };
}

AsyncResult<TrackId> RtcPipelineEngine::publish(
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

AsyncResult<TrackId> RtcPipelineEngine::publish_graph(TrackDescriptor descriptor,
                                                      PublishGraphFactory factory,
                                                      FailureCleanup cleanup) {
  require_owner();
  if (!factory) {
    throw std::invalid_argument("RTC engine requires a graph factory");
  }
  if (pending_.size() >= 1024) {
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                   "RTC graph operation queue exhausted");
  }
  auto promise = std::make_shared<CompletionSource<TrackId>>();
  auto result = promise->get_result();
  auto operation = room_->publish(descriptor);
  auto pending = std::make_shared<Pending>();
  pending->id = descriptor.id;
  pending->publication = true;
  pending->cancel = [promise, cleanup = std::move(cleanup)](auto failure) {
    if (cleanup) {
      cleanup(failure);
    }
    promise->set_exception(failure);
  };
  pending_.push_back(pending);
  std::weak_ptr<int> lifetime = lifetime_;
  std::move(operation).then(dispatch_, [this, lifetime, pending, promise,
                                        factory = std::move(factory)](
                                           AsyncResult<std::shared_ptr<PublishedTrack>> ready) {
    if (lifetime.expired() || pending->settled) {
      try {
        ready.get()->close();
      } catch (...) {
      }
      return;
    }
    pending->settled = true;
    try {
      auto endpoint = ready.get();
      try {
        auto graph = std::make_shared<PublishPipeline>(endpoint, factory, observer(endpoint));
        graph->start();
        const auto id = endpoint->descriptor().id;
        {
          std::lock_guard lock(mutex_);
          if (!publications_.emplace(id, std::move(graph)).second) {
            throw std::logic_error("RTC track graph already exists");
          }
        }
        promise->set_value(id);
      } catch (...) {
        const auto error = normalized_failure(
            std::current_exception(), {"create_graph", pending->id, {}, endpoint->snapshot().epoch},
            ErrorDomain::media, ErrorCode::processing_failed);
        endpoint->report_failure(error);
        endpoint->close();
        std::rethrow_exception(error);
      }
    } catch (...) {
      const auto error =
          normalized_failure(std::current_exception(), {"create_graph", pending->id, {}, 0},
                             ErrorDomain::media, ErrorCode::processing_failed);
      pending->cancel(error);
      if (failure_handler_) {
        auto info = describe_failure(error);
        failure_handler_({pending->id, pending->publication, info->message, std::move(info)});
      }
    }
    std::erase(pending_, pending);
  });
  return result;
}

AsyncResult<TrackId> RtcPipelineEngine::subscribe(
    TrackDescriptor descriptor, std::shared_ptr<graphflow::core::GraphNode> consumer) {
  if (!consumer)
    throw std::invalid_argument("RTC engine requires a subscription consumer node");
  return subscribe_graph(std::move(descriptor), [consumer](auto source) {
    TrackGraphSpec spec;
    spec.nodes.push_back(consumer);
    spec.connections.push_back(TrackGraphSpec::connect<nodes::EncodedFrameEvent>(source, consumer));
    return spec;
  });
}

AsyncResult<TrackId> RtcPipelineEngine::subscribe_graph(TrackDescriptor descriptor,
                                                        SubscribeGraphFactory factory,
                                                        FailureCleanup cleanup) {
  require_owner();
  if (!factory) {
    throw std::invalid_argument("RTC engine requires a graph factory");
  }
  if (pending_.size() >= 1024) {
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                   "RTC graph operation queue exhausted");
  }
  auto promise = std::make_shared<CompletionSource<TrackId>>();
  auto result = promise->get_result();
  auto operation = room_->subscribe(descriptor);
  auto pending = std::make_shared<Pending>();
  pending->id = descriptor.id;
  pending->publication = false;
  pending->cancel = [promise, cleanup = std::move(cleanup)](auto failure) {
    if (cleanup) {
      cleanup(failure);
    }
    promise->set_exception(failure);
  };
  pending_.push_back(pending);
  std::weak_ptr<int> lifetime = lifetime_;
  std::move(operation).then(dispatch_, [this, lifetime, pending, promise,
                                        factory = std::move(factory)](
                                           AsyncResult<std::shared_ptr<SubscribedTrack>> ready) {
    if (lifetime.expired() || pending->settled) {
      try {
        ready.get()->close();
      } catch (...) {
      }
      return;
    }
    pending->settled = true;
    try {
      auto endpoint = ready.get();
      try {
        auto graph = std::make_shared<SubscribePipeline>(endpoint, factory, observer(endpoint));
        graph->start();
        const auto id = endpoint->descriptor().id;
        {
          std::lock_guard lock(mutex_);
          if (!subscriptions_.emplace(id, std::move(graph)).second) {
            throw std::logic_error("RTC track graph already exists");
          }
        }
        promise->set_value(id);
      } catch (...) {
        const auto error = normalized_failure(
            std::current_exception(), {"create_graph", pending->id, {}, endpoint->snapshot().epoch},
            ErrorDomain::media, ErrorCode::processing_failed);
        endpoint->report_failure(error);
        endpoint->close();
        std::rethrow_exception(error);
      }
    } catch (...) {
      const auto error =
          normalized_failure(std::current_exception(), {"create_graph", pending->id, {}, 0},
                             ErrorDomain::media, ErrorCode::processing_failed);
      pending->cancel(error);
      if (failure_handler_) {
        auto info = describe_failure(error);
        failure_handler_({pending->id, pending->publication, info->message, std::move(info)});
      }
    }
    std::erase(pending_, pending);
  });
  return result;
}

void RtcPipelineEngine::remove_graph(const TrackId& id, bool publication) {
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
  if (publish)
    publish->close();
  if (subscribe)
    subscribe->close();
}

AsyncResult<void> RtcPipelineEngine::unpublish(const TrackId& id) {
  require_owner();
  cancel_pending(id, true);
  remove_graph(id, true);
  return room_->unpublish(id);
}

AsyncResult<void> RtcPipelineEngine::unsubscribe(const TrackId& id) {
  require_owner();
  cancel_pending(id, false);
  remove_graph(id, false);
  return room_->unsubscribe(id);
}

void RtcPipelineEngine::cancel_pending(const TrackId& id, bool publication) {
  for (auto it = pending_.begin(); it != pending_.end();) {
    auto& pending = **it;
    if (pending.id == id && pending.publication == publication) {
      pending.settled = true;
      pending.cancel(std::make_exception_ptr(
          SDKError(ErrorDomain::lifecycle, ErrorCode::cancelled, "RTC graph creation cancelled")));
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
}

void RtcPipelineEngine::set_failure_handler(std::function<void(GraphFailure)> handler) {
  require_owner();
  failure_handler_ = std::move(handler);
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

void RtcPipelineEngine::stopped_graph(const std::shared_ptr<PublishedTrack>& endpoint,
                                      std::exception_ptr failure) {
  require_owner();
  const auto id = endpoint->descriptor().id;
  std::shared_ptr<PublishPipeline> graph;
  {
    std::lock_guard lock(mutex_);
    auto it = publications_.find(id);
    if (it == publications_.end() || it->second->node().track() != endpoint) {
      return;
    }
    graph = it->second;
  }
  // A failure reported after a normal terminal notification takes precedence.
  if (auto reported = graph->failure()) {
    failure = reported;
  }
  if (failure) {
    graph->notify_failure(failure);
  }
  remove_graph(id, true);
  if (failure && failure_handler_) {
    auto info = describe_failure(failure);
    failure_handler_({id, true, info->message, std::move(info)});
  }
}

void RtcPipelineEngine::stopped_graph(const std::shared_ptr<SubscribedTrack>& endpoint,
                                      std::exception_ptr failure) {
  require_owner();
  const auto id = endpoint->descriptor().id;
  std::shared_ptr<SubscribePipeline> graph;
  {
    std::lock_guard lock(mutex_);
    auto it = subscriptions_.find(id);
    if (it == subscriptions_.end() || it->second->node().track() != endpoint) {
      return;
    }
    graph = it->second;
  }
  // A failure reported after a normal terminal notification takes precedence.
  if (auto reported = graph->failure()) {
    failure = reported;
  }
  if (failure) {
    graph->notify_failure(failure);
  }
  remove_graph(id, false);
  if (failure && failure_handler_) {
    auto info = describe_failure(failure);
    failure_handler_({id, false, info->message, std::move(info)});
  }
}

void RtcPipelineEngine::clear() {
  if (std::this_thread::get_id() != owner_)
    throw std::logic_error("RTC graph close requires its owner thread");
  for (auto& pending : pending_) {
    pending->settled = true;
    pending->cancel(closed_error());
  }
  pending_.clear();
  std::map<TrackId, std::shared_ptr<PublishPipeline>> publications;
  std::map<TrackId, std::shared_ptr<SubscribePipeline>> subscriptions;
  {
    std::lock_guard lock(mutex_);
    publications.swap(publications_);
    subscriptions.swap(subscriptions_);
  }
  for (auto& [id, graph] : publications)
    graph->close();
  for (auto& [id, graph] : subscriptions)
    graph->close();
}
}  // namespace livekit::sdk::rtc::engine
