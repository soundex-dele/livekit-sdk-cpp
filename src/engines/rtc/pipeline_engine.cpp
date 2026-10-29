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

RtcPipelineEngine::RtcPipelineEngine(RoomEngine& room) : room_(&room) {}

RtcPipelineEngine::~RtcPipelineEngine() {
  // Destruction, like all graph lifecycle operations, must follow producer
  // quiescence. RoomEngine destruction supplies the final native barrier.
  for (auto& pending : pending_)
    pending.cancel(closed_error());
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

std::future<TrackId> RtcPipelineEngine::publish(
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

std::future<TrackId> RtcPipelineEngine::publish_graph(TrackDescriptor descriptor,
                                                      PublishGraphFactory factory,
                                                      FailureCleanup cleanup) {
  require_owner();
  if (!factory)
    throw std::invalid_argument("RTC engine requires a publication graph factory");
  if (pending_.size() >= 1024)
    throw std::runtime_error("RTC graph operation queue exhausted");
  auto promise = std::make_shared<std::promise<TrackId>>();
  auto result = promise->get_future();
  auto operation =
      std::make_shared<std::future<std::shared_ptr<PublishedTrack>>>(room_->publish(descriptor));
  pending_.push_back({descriptor.id, true,
                      [this, promise, operation, factory = std::move(factory)] {
                        if (operation->wait_for(std::chrono::seconds::zero()) !=
                            std::future_status::ready)
                          return false;
                        auto endpoint = operation->get();
                        try {
                          auto graph = std::make_shared<PublishPipeline>(endpoint, factory);
                          graph->start();
                          const auto id = endpoint->descriptor().id;
                          {
                            std::lock_guard lock(mutex_);
                            if (!publications_.emplace(id, std::move(graph)).second)
                              throw std::logic_error("RTC publication graph already exists");
                          }
                          promise->set_value(id);
                        } catch (...) {
                          endpoint->close();
                          throw;
                        }
                        return true;
                      },
                      [promise, cleanup = std::move(cleanup)](auto failure) {
                        if (cleanup) {
                          cleanup(failure);
                        }
                        promise->set_exception(failure);
                      }});
  return result;
}

std::future<TrackId> RtcPipelineEngine::subscribe(
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

std::future<TrackId> RtcPipelineEngine::subscribe_graph(TrackDescriptor descriptor,
                                                        SubscribeGraphFactory factory,
                                                        FailureCleanup cleanup) {
  require_owner();
  if (!factory)
    throw std::invalid_argument("RTC engine requires a subscription graph factory");
  if (pending_.size() >= 1024)
    throw std::runtime_error("RTC graph operation queue exhausted");
  auto promise = std::make_shared<std::promise<TrackId>>();
  auto result = promise->get_future();
  auto operation =
      std::make_shared<std::future<std::shared_ptr<SubscribedTrack>>>(room_->subscribe(descriptor));
  pending_.push_back({descriptor.id, false,
                      [this, promise, operation, factory = std::move(factory)] {
                        if (operation->wait_for(std::chrono::seconds::zero()) !=
                            std::future_status::ready)
                          return false;
                        auto endpoint = operation->get();
                        try {
                          auto graph = std::make_shared<SubscribePipeline>(endpoint, factory);
                          graph->start();
                          const auto id = endpoint->descriptor().id;
                          {
                            std::lock_guard lock(mutex_);
                            if (!subscriptions_.emplace(id, std::move(graph)).second)
                              throw std::logic_error("RTC subscription graph already exists");
                          }
                          promise->set_value(id);
                        } catch (...) {
                          endpoint->close();
                          throw;
                        }
                        return true;
                      },
                      [promise, cleanup = std::move(cleanup)](auto failure) {
                        if (cleanup) {
                          cleanup(failure);
                        }
                        promise->set_exception(failure);
                      }});
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

std::future<void> RtcPipelineEngine::unpublish(const TrackId& id) {
  require_owner();
  cancel_pending(id, true);
  remove_graph(id, true);
  return room_->unpublish(id);
}

std::future<void> RtcPipelineEngine::unsubscribe(const TrackId& id) {
  require_owner();
  cancel_pending(id, false);
  remove_graph(id, false);
  return room_->unsubscribe(id);
}

void RtcPipelineEngine::cancel_pending(const TrackId& id, bool publication) {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->id == id && it->publication == publication) {
      it->cancel(std::make_exception_ptr(
          SDKError(ErrorDomain::lifecycle, ErrorCode::cancelled, "RTC graph creation cancelled")));
      it = pending_.erase(it);
    } else
      ++it;
  }
}

void RtcPipelineEngine::failed_graph(const TrackId& id, bool publication, std::string message) {
  remove_graph(id, publication);
  if (failure_handler_) {
    failure_handler_({id, publication, std::move(message)});
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

void RtcPipelineEngine::complete_operations() {
  require_owner();
  for (auto it = pending_.begin(); it != pending_.end();) {
    bool completed = false;
    try {
      completed = it->complete();
    } catch (...) {
      it->cancel(std::current_exception());
      completed = true;
    }
    if (completed)
      it = pending_.erase(it);
    else
      ++it;
  }
}

void RtcPipelineEngine::check_health() {
  require_owner();
  std::vector<std::pair<TrackId, std::shared_ptr<PublishPipeline>>> publications;
  std::vector<std::pair<TrackId, std::shared_ptr<SubscribePipeline>>> subscriptions;
  {
    std::lock_guard lock(mutex_);
    publications.assign(publications_.begin(), publications_.end());
    subscriptions.assign(subscriptions_.begin(), subscriptions_.end());
  }
  for (auto& [id, graph] : publications) {
    try {
      const auto status = graph->node().track()->snapshot();
      if (status.state == TrackState::closed)
        remove_graph(id, true);
      else if (status.state == TrackState::failed)
        failed_graph(id, true, status.error);
      else
        graph->check();
    } catch (const std::exception& e) {
      failed_graph(id, true, e.what());
    } catch (...) {
      failed_graph(id, true, "RTC feedback consumer threw an unknown exception");
    }
  }
  for (auto& [id, graph] : subscriptions) {
    try {
      const auto status = graph->node().track()->snapshot();
      if (status.state == TrackState::closed)
        remove_graph(id, false);
      else if (status.state == TrackState::failed)
        failed_graph(id, false, status.error);
      else
        graph->check();
    } catch (const std::exception& e) {
      failed_graph(id, false, e.what());
    } catch (...) {
      failed_graph(id, false, "RTC media consumer threw an unknown exception");
    }
  }
}

void RtcPipelineEngine::clear() {
  if (std::this_thread::get_id() != owner_)
    throw std::logic_error("RTC graph close requires its owner thread");
  for (auto& pending : pending_)
    pending.cancel(closed_error());
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
