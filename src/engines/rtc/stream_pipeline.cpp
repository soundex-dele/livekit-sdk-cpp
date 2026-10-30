#include "livekit/rtc/stream_pipeline.hpp"
#include "backends/rtc/control_context.hpp"
#include <atomic>
#include <stdexcept>
#include <utility>

namespace livekit::sdk::rtc::engine {
// Node handlers retain only this shared report state, not the pipeline or its
// owner. Owner teardown drains source workers before releasing the registry.
struct GraphObservation {
  std::atomic<bool> active{true};
  mutable std::mutex mutex;
  std::exception_ptr cause;
  GraphStopped stopped;
  std::function<Epoch()> epoch;
  TrackId id;

  std::exception_ptr failure() const {
    std::lock_guard lock(mutex);
    return cause;
  }

  void report(std::exception_ptr value, const std::string& node) {
    if (!active) {
      return;
    }
    {
      std::lock_guard lock(mutex);
      if (cause) {
        return;
      }
      cause = normalized_failure(value, {"graph", id, node, epoch()}, ErrorDomain::media,
                                 ErrorCode::processing_failed);
      value = cause;
    }
    if (stopped) {
      detail::CallbackScope callback_scope;
      stopped(value);
    }
  }

  void terminal() {
    if (active && stopped) {
      detail::CallbackScope callback_scope;
      stopped(failure());
    }
  }
};
namespace {
std::atomic<uint64_t> next_graph{0};

std::string name(const char* role, const TrackId& id) {
  return std::string("rtc.") + role + "." + std::to_string(++next_graph) + "." + id;
}

template <class Endpoint>
std::shared_ptr<GraphObservation> observe(const std::shared_ptr<Endpoint>& endpoint,
                                          GraphStopped stopped) {
  auto result = std::make_shared<GraphObservation>();
  result->id = endpoint->descriptor().id;
  result->stopped = std::move(stopped);
  result->epoch = [weak = std::weak_ptr<Endpoint>(endpoint)] {
    auto track = weak.lock();
    return track ? track->snapshot().epoch : Epoch{};
  };
  return result;
}

template <class Endpoint>
std::exception_ptr startup_failure(const std::shared_ptr<GraphObservation>& observation,
                                   const std::shared_ptr<Endpoint>& endpoint) {
  if (auto reported = observation->failure()) {
    return reported;
  }
  const auto state = endpoint->snapshot();
  // A transport may fail after ACK but before this graph starts. Preserve that
  // cause instead of replacing it with a generic node-start failure.
  if (state.failure) {
    return std::make_exception_ptr(SDKError(state.failure));
  }
  return normalized_failure(std::current_exception(),
                            {"start_graph", endpoint->descriptor().id, {}, state.epoch},
                            ErrorDomain::media, ErrorCode::processing_failed);
}

void build(graphflow::engine::GraphEngine& graph,
           std::vector<std::shared_ptr<graphflow::core::GraphNode>> nodes,
           std::vector<graphflow::engine::NodeConnection> connections,
           const std::shared_ptr<GraphObservation>& observation) {
  for (const auto& node : nodes) {
    node->setFailureHandler([observation, name = node->getName()](auto failure) {
      observation->report(failure, name);
    });
  }
  if (!graph.loadFromNodes(nodes, connections) || !graph.init())
    throw std::runtime_error("Unable to prepare RTC track graph");
}

void quiesce(TrackGraphSpec& extension) {
  // Move first so partial activation and repeated close invoke this exactly once.
  if (auto stop = std::exchange(extension.quiesce, {})) {
    detail::CallbackScope scope;
    stop();
  }
}
}  // namespace

class PublishPipeline::Input final : public graphflow::core::GraphNode {
 public:
  explicit Input(const TrackId& id) : GraphNode(name("input", id), nullptr) {}

  WriteStatus push(EncodedFrame frame) {
    nodes::EncodedFrameEvent event(std::move(frame));
    event.receipt = std::make_shared<std::atomic<WriteStatus>>(WriteStatus::not_ready);
    detail::CallbackScope callback_scope;
    publish(event);
    return event.receipt->load();
  }
};

PublishPipeline::PublishPipeline(std::shared_ptr<PublishedTrack> track,
                                 std::shared_ptr<graphflow::core::GraphNode> feedback_sink)
    : PublishPipeline(
          std::move(track), PublishGraphFactory([feedback_sink](auto sink) {
            TrackGraphSpec spec;
            if (feedback_sink) {
              spec.nodes.push_back(feedback_sink);
              spec.connections.push_back(
                  TrackGraphSpec::connect<nodes::SenderFeedbackEvent>(sink, feedback_sink));
            }
            return spec;
          })) {}

PublishPipeline::PublishPipeline(std::shared_ptr<PublishedTrack> track, PublishGraphFactory factory,
                                 GraphStopped stopped) {
  if (!track || !factory)
    throw std::invalid_argument("RTC publication requires endpoint and graph factory");
  input_ = std::make_shared<Input>(track->descriptor().id);
  sink_ = std::make_shared<nodes::RtcPublishNode>(name("publish", track->descriptor().id), track);
  {
    detail::CallbackScope scope;
    extension_ = factory(sink_);
  }
  observation_ = observe(track, std::move(stopped));
  sink_->set_terminal_handler([state = observation_] { state->terminal(); });
  auto graph_nodes = extension_.nodes;
  auto connections = extension_.connections;
  graph_nodes.insert(graph_nodes.begin(), sink_);
  graph_nodes.push_back(input_);
  connections.push_back(TrackGraphSpec::connect<nodes::EncodedFrameEvent>(input_, sink_));
  build(graph_, std::move(graph_nodes), std::move(connections), observation_);
}

PublishPipeline::~PublishPipeline() {
  (void)close_locked();
  graph_.release();
}

void PublishPipeline::start() {
  auto control = sink_->track()->operation_control();
  detail::OperationGuard guard(*control);
  start_locked();
}

void PublishPipeline::start_locked() {
  {
    std::lock_guard lock(mutex_);
    if (closed_)
      throw std::logic_error("Closed RTC pipeline cannot restart");
    if (running_)
      return;
  }
  try {
    if (!graph_.start())
      throw std::runtime_error("RTC publication graph start failed");
    // Deliver current epoch/bitrate before producers can emit their first frame.
    sink_->activate();
    if (extension_.activate) {
      detail::CallbackScope scope;
      extension_.activate();
    }
    std::lock_guard lock(mutex_);
    if (auto error = observation_->failure()) {
      std::rethrow_exception(error);
    }
    running_ = true;
  } catch (...) {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    const auto error = startup_failure(observation_, sink_->track());
    notify_failure(error);
    sink_->quiesce();
    quiesce(extension_);
    graph_.stop();
    (void)sink_->track()->close_locked();
    std::rethrow_exception(error);
  }
}

WriteStatus PublishPipeline::write(EncodedFrame frame) {
  std::lock_guard lock(mutex_);
  if (closed_ || observation_->failure())
    return WriteStatus::closed;
  if (!running_)
    return WriteStatus::not_ready;
  return input_->push(std::move(frame));
}

std::exception_ptr PublishPipeline::failure() const {
  return observation_->failure();
}

void PublishPipeline::notify_failure(std::exception_ptr failure) {
  sink_->track()->report_failure(failure);
  if (extension_.on_failure) {
    detail::CallbackScope scope;
    extension_.on_failure(failure);
  }
}

Result<void> PublishPipeline::close() {
  return detail::control_boundary([&] {
    auto control = sink_->track()->operation_control();
    detail::OperationGuard guard(*control);
    return close_locked();
  });
}

Result<void> PublishPipeline::close_locked() {
  observation_->active = false;
  bool stop = false;
  {
    std::lock_guard lock(mutex_);
    if (!closed_) {
      running_ = false;
      closed_ = true;
      stop = true;
    }
  }
  // A producer can be completing write() while quiesce joins its thread.
  // Admission is closed above; never hold the ingress mutex during that join.
  if (stop) {
    sink_->quiesce();
    quiesce(extension_);
    graph_.stop();
  }
  return sink_->track()->close_locked();
}

SubscribePipeline::SubscribePipeline(std::shared_ptr<SubscribedTrack> track,
                                     std::shared_ptr<graphflow::core::GraphNode> consumer)
    : SubscribePipeline(std::move(track), SubscribeGraphFactory([consumer](auto source) {
                          if (!consumer)
                            throw std::invalid_argument("RTC subscription requires consumer");
                          TrackGraphSpec spec;
                          spec.nodes.push_back(consumer);
                          spec.connections.push_back(
                              TrackGraphSpec::connect<nodes::EncodedFrameEvent>(source, consumer));
                          return spec;
                        })) {}

SubscribePipeline::SubscribePipeline(std::shared_ptr<SubscribedTrack> track,
                                     SubscribeGraphFactory factory, GraphStopped stopped) {
  if (!track || !factory)
    throw std::invalid_argument("RTC subscription requires endpoint and graph factory");
  source_ =
      std::make_shared<nodes::RtcSubscribeNode>(name("subscribe", track->descriptor().id), track);
  {
    detail::CallbackScope scope;
    extension_ = factory(source_);
  }
  observation_ = observe(track, std::move(stopped));
  source_->set_terminal_handler([state = observation_] { state->terminal(); });
  auto graph_nodes = extension_.nodes;
  graph_nodes.push_back(source_);
  build(graph_, std::move(graph_nodes), extension_.connections, observation_);
}

SubscribePipeline::~SubscribePipeline() {
  (void)close_locked();
  graph_.release();
}

void SubscribePipeline::start() {
  auto control = source_->track()->operation_control();
  detail::OperationGuard guard(*control);
  start_locked();
}

void SubscribePipeline::start_locked() {
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      throw std::logic_error("Closed RTC pipeline cannot restart");
    }
    if (running_) {
      return;
    }
  }
  try {
    if (!graph_.start()) {
      throw std::runtime_error("RTC subscription graph start failed");
    }
    if (extension_.activate) {
      {
        detail::CallbackScope scope;
        extension_.activate();
      }
    }
    source_->activate();
    std::lock_guard lock(mutex_);
    if (auto error = observation_->failure()) {
      std::rethrow_exception(error);
    }
    running_ = true;
  } catch (...) {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    const auto error = startup_failure(observation_, source_->track());
    notify_failure(error);
    source_->quiesce();
    quiesce(extension_);
    graph_.stop();
    (void)source_->track()->close_locked();
    std::rethrow_exception(error);
  }
}

std::exception_ptr SubscribePipeline::failure() const {
  return observation_->failure();
}

void SubscribePipeline::notify_failure(std::exception_ptr failure) {
  source_->track()->report_failure(failure);
  if (extension_.on_failure) {
    detail::CallbackScope scope;
    extension_.on_failure(failure);
  }
}

Result<void> SubscribePipeline::close() {
  return detail::control_boundary([&] {
    auto control = source_->track()->operation_control();
    detail::OperationGuard guard(*control);
    return close_locked();
  });
}

Result<void> SubscribePipeline::close_locked() {
  observation_->active = false;
  bool stop = false;
  {
    std::lock_guard lock(mutex_);
    if (!closed_) {
      running_ = false;
      closed_ = true;
      stop = true;
    }
  }
  if (stop) {
    // Stop and join ingress before any downstream quiescence or graph teardown.
    // No pipeline lock may be held across an in-flight consumer callback.
    source_->quiesce();
    quiesce(extension_);
    graph_.stop();
  }
  return source_->track()->close_locked();
}
}  // namespace livekit::sdk::rtc::engine
