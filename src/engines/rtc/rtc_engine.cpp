#include "livekit/rtc/engine.hpp"
#include "backends/rtc/room_transport.hpp"
#include "livekit/error.hpp"

namespace livekit::sdk::rtc::engine {
namespace {
std::exception_ptr closed_error() {
  return std::make_exception_ptr(
      SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed, "RTC engine closed"));
}
}  // namespace

RtcEngine::RtcEngine(std::unique_ptr<detail::RoomTransport> transport)
    : RtcEngine(std::make_unique<RoomSession>(std::move(transport))) {}

RtcEngine::RtcEngine(std::unique_ptr<RoomSession> room) : room_(std::move(room)) {
  if (!room_)
    throw std::invalid_argument("RTC engine requires a room backend");
}

RtcEngine::~RtcEngine() {
  // Destruction, like all graph lifecycle operations, must follow producer
  // quiescence. RoomSession destruction supplies the final native barrier.
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

void RtcEngine::require_owner() const {
  if (std::this_thread::get_id() != owner_)
    throw std::logic_error("RTC graph control requires its owner thread");
  if (closed_)
    throw std::logic_error("RTC engine is closed");
}

std::future<void> RtcEngine::join(RoomOptions options) {
  require_owner();
  return room_->join(std::move(options));
}

std::future<void> RtcEngine::set_muted(const TrackId& id, bool muted, ControlOptions options) {
  require_owner();
  return room_->set_muted(id, muted, options);
}

std::future<void> RtcEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                                ControlOptions options) {
  require_owner();
  return room_->set_receive_demand(id, demand, options);
}

std::future<TrackId> RtcEngine::publish(TrackDescriptor descriptor,
                                        std::shared_ptr<graphflow::core::GraphNode> feedback_sink) {
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

std::future<TrackId> RtcEngine::publish_graph(TrackDescriptor descriptor,
                                              PublishGraphFactory factory) {
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
                      [promise](auto failure) { promise->set_exception(failure); }});
  return result;
}

std::future<TrackId> RtcEngine::subscribe(TrackDescriptor descriptor,
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

std::future<TrackId> RtcEngine::subscribe_graph(TrackDescriptor descriptor,
                                                SubscribeGraphFactory factory) {
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
                      [promise](auto failure) { promise->set_exception(failure); }});
  return result;
}

void RtcEngine::remove_graph(const TrackId& id, bool publication) {
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

std::future<void> RtcEngine::unpublish(const TrackId& id) {
  require_owner();
  cancel_pending(id, true);
  remove_graph(id, true);
  return room_->unpublish(id);
}

std::future<void> RtcEngine::unsubscribe(const TrackId& id) {
  require_owner();
  cancel_pending(id, false);
  remove_graph(id, false);
  return room_->unsubscribe(id);
}

void RtcEngine::cancel_pending(const TrackId& id, bool publication) {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->id == id && it->publication == publication) {
      it->cancel(std::make_exception_ptr(
          SDKError(ErrorDomain::lifecycle, ErrorCode::cancelled, "RTC graph creation cancelled")));
      it = pending_.erase(it);
    } else
      ++it;
  }
}

void RtcEngine::failed_graph(const TrackId& id, bool publication, std::string message) {
  remove_graph(id, publication);
  if (failures_.size() >= 256)
    failures_.pop_front();
  failures_.push_back({id, publication, std::move(message)});
}

std::optional<RtcEngine::GraphFailure> RtcEngine::next_failure() {
  if (std::this_thread::get_id() != owner_)
    throw std::logic_error("RTC failures require the owner thread");
  if (failures_.empty())
    return {};
  auto failure = std::move(failures_.front());
  failures_.pop_front();
  return failure;
}

WriteStatus RtcEngine::write(const TrackId& id, EncodedFrame frame) {
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

TrackSnapshot RtcEngine::publication(const TrackId& id) const {
  std::lock_guard lock(mutex_);
  auto it = publications_.find(id);
  if (it == publications_.end())
    throw std::out_of_range("Unknown RTC engine publication");
  return it->second->node().track()->snapshot();
}

SenderFeedback RtcEngine::feedback(const TrackId& id) const {
  std::lock_guard lock(mutex_);
  auto it = publications_.find(id);
  if (it == publications_.end())
    throw std::out_of_range("Unknown RTC engine publication");
  return it->second->node().track()->feedback();
}

void RtcEngine::poll(size_t receive_frames_per_track) {
  require_owner();
  for (auto it = pending_.begin(); it != pending_.end();) {
    bool completed = false;
    try {
      completed = it->poll();
    } catch (...) {
      it->cancel(std::current_exception());
      completed = true;
    }
    if (completed)
      it = pending_.erase(it);
    else
      ++it;
  }
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
        graph->poll_feedback();
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
        graph->poll(receive_frames_per_track);
    } catch (const std::exception& e) {
      failed_graph(id, false, e.what());
    } catch (...) {
      failed_graph(id, false, "RTC media consumer threw an unknown exception");
    }
  }
}

std::future<void> RtcEngine::close() {
  if (std::this_thread::get_id() != owner_)
    throw std::logic_error("RTC graph close requires its owner thread");
  closed_ = true;
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
  return room_->leave();
}
}  // namespace livekit::sdk::rtc::engine
