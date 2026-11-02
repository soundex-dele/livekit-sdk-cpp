#include "media_graph/failure.hpp"
#include "livekit/rtc/stream_pipeline.hpp"
#include "backends/rtc/control_context.hpp"
#include <atomic>

#include <utility>

namespace livekit::sdk::rtc::engine {
// Node handlers retain only this shared report state, not the pipeline or its
// owner. Owner teardown drains source workers before releasing the registry.
struct GraphObservation {
  std::atomic<bool> active{true};
  mutable std::mutex mutex;
  Failure cause;
  GraphStopped stopped;
  std::function<Epoch()> epoch;
  TrackId id;

  Failure failure() const {
    std::lock_guard lock(mutex);
    return cause;
  }

  void report(Failure value, const std::string& node) {
    if (!active) {
      return;
    }
    {
      std::lock_guard lock(mutex);
      if (cause) {
        return;
      }
      cause = with_context(value, {"graph", id, node, epoch ? epoch() : Epoch{}});
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
Failure startup_failure(const std::shared_ptr<GraphObservation>& observation,
                        const std::shared_ptr<Endpoint>& endpoint) {
  if (auto reported = observation->failure()) {
    return reported;
  }
  const auto state = endpoint->snapshot();
  // A transport may fail after ACK but before this graph starts. Preserve that
  // cause instead of replacing it with a generic node-start failure.
  if (state.failure) {
    return state.failure;
  }
  return make_failure(ErrorDomain::media, ErrorCode::processing_failed,
                      "Unable to start RTC track graph", false, 0,
                      {"start_graph", endpoint->descriptor().id, {}, state.epoch});
}

Result<void> build(graphflow::engine::GraphEngine& graph,
                   std::vector<std::shared_ptr<graphflow::core::GraphNode>> nodes,
                   std::vector<graphflow::engine::NodeConnection> connections,
                   const std::shared_ptr<GraphObservation>& observation) {
  for (const auto& node : nodes) {
    if (!node) {
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                                "RTC graph contains a null node"));
    }
    node->setErrorHandler([observation, name = node->getName()](const auto& error) {
      observation->report(livekit::sdk::detail::graph_failure(error), name);
    });
  }
  if (!graph.loadFromNodes(nodes, connections) || !graph.init()) {
    return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::processing_failed,
                                              "Unable to prepare RTC track graph"));
  }
  return Result<void>::success();
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
  observation_ = std::make_shared<GraphObservation>();
  if (!track || !factory) {
    observation_->report(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                      "RTC pipeline requires endpoint and graph factory"),
                         {});
    return;
  }
  input_ = std::make_shared<Input>(track->descriptor().id);
  sink_ = std::make_shared<nodes::RtcPublishNode>(name("publish", track->descriptor().id), track);
  {
    detail::CallbackScope scope;
    extension_ = factory(sink_);
  }
  observation_ = observe(track, std::move(stopped));
  sink_->set_terminal_handler([state = observation_] { state->terminal(); });
  if (auto prepared = prepare(); !prepared) {
    observation_->report(prepared.failure_record(), {});
  }
}

PublishPipeline::PublishPipeline(std::shared_ptr<nodes::RtcPublishNode> node, TrackGraphSpec spec,
                                 GraphStopped stopped)
    : sink_(std::move(node)), extension_(std::move(spec)), owns_endpoint_(false) {
  input_ = std::make_shared<Input>(sink_->track()->descriptor().id);
  observation_ = observe(sink_->track(), std::move(stopped));
  sink_->set_terminal_handler([state = observation_] { state->terminal(); });
}

Result<void> PublishPipeline::prepare() {
  if (extension_.failure) {
    return Result<void>::failure(extension_.failure);
  }
  auto graph_nodes = extension_.nodes;
  auto connections = extension_.connections;
  graph_nodes.insert(graph_nodes.begin(), sink_);
  graph_nodes.push_back(input_);
  connections.push_back(TrackGraphSpec::connect<nodes::EncodedFrameEvent>(input_, sink_));
  auto prepared = build(graph_, std::move(graph_nodes), std::move(connections), observation_);
  prepared_ = bool(prepared);
  return prepared;
}

PublishPipeline::~PublishPipeline() {
  (void)close();
  graph_.release();
}

Result<void> PublishPipeline::start() {
  if (auto failure = observation_->failure()) {
    return Result<void>::failure(failure);
  }
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                                "Closed RTC pipeline cannot restart"));
    }
    if (running_) {
      return Result<void>::success();
    }
  }
  auto started = [&]() -> Result<void> {
    if (!prepared_) {
      if (auto prepared = prepare(); !prepared) {
        return prepared;
      }
    }
    if (!graph_.start()) {
      return Result<void>::failure(startup_failure(observation_, sink_->track()));
    }
    if (auto activated = sink_->activate(); !activated) {
      return activated;
    }
    if (extension_.activate) {
      detail::CallbackScope scope;
      if (auto activated = extension_.activate(); !activated) {
        return activated;
      }
    }
    std::lock_guard lock(mutex_);
    if (auto failure = observation_->failure()) {
      return Result<void>::failure(failure);
    }
    running_ = true;
    return Result<void>::success();
  }();
  if (!started) {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    const auto failure = started.failure_record();
    sink_->quiesce();
    quiesce(extension_);
    graph_.stop();
    observation_->report(failure, {});
    notify_failure(failure);
    if (owns_endpoint_) {
      (void)sink_->track()->close();
    }
  }
  return started;
}

WriteStatus PublishPipeline::write(EncodedFrame frame) {
  std::lock_guard lock(mutex_);
  if (closed_ || observation_->failure())
    return WriteStatus::closed;
  if (!running_)
    return WriteStatus::not_ready;
  return input_->push(std::move(frame));
}

Failure PublishPipeline::failure() const {
  return observation_->failure();
}

void PublishPipeline::notify_failure(Failure failure) {
  if (failure_notified_) {
    return;
  }
  failure_notified_ = true;
  if (extension_.on_failure) {
    detail::CallbackScope scope;
    extension_.on_failure(failure);
  }
}

ErrorCode PublishPipeline::close() {
  if (!sink_) {
    return ErrorCode::ok;
  }
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
  return stop && owns_endpoint_ ? sink_->track()->close() : ErrorCode::ok;
}

SubscribePipeline::SubscribePipeline(std::shared_ptr<SubscribedTrack> track,
                                     std::shared_ptr<graphflow::core::GraphNode> consumer)
    : SubscribePipeline(std::move(track), SubscribeGraphFactory([consumer](auto source) {
                          TrackGraphSpec spec;
                          if (!consumer) {
                            spec.failure =
                                make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                             "RTC subscription requires consumer");
                            return spec;
                          }
                          spec.nodes.push_back(consumer);
                          spec.connections.push_back(
                              TrackGraphSpec::connect<nodes::EncodedFrameEvent>(source, consumer));
                          return spec;
                        })) {}

SubscribePipeline::SubscribePipeline(std::shared_ptr<SubscribedTrack> track,
                                     SubscribeGraphFactory factory, GraphStopped stopped) {
  observation_ = std::make_shared<GraphObservation>();
  if (!track || !factory) {
    observation_->report(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                      "RTC pipeline requires endpoint and graph factory"),
                         {});
    return;
  }
  source_ =
      std::make_shared<nodes::RtcSubscribeNode>(name("subscribe", track->descriptor().id), track);
  {
    detail::CallbackScope scope;
    extension_ = factory(source_);
  }
  observation_ = observe(track, std::move(stopped));
  source_->set_terminal_handler([state = observation_] { state->terminal(); });
  if (auto prepared = prepare(); !prepared) {
    observation_->report(prepared.failure_record(), {});
  }
}

SubscribePipeline::SubscribePipeline(std::shared_ptr<nodes::RtcSubscribeNode> node,
                                     TrackGraphSpec spec, GraphStopped stopped)
    : source_(std::move(node)), extension_(std::move(spec)), owns_endpoint_(false) {
  observation_ = observe(source_->track(), std::move(stopped));
  source_->set_terminal_handler([state = observation_] { state->terminal(); });
}

Result<void> SubscribePipeline::prepare() {
  if (extension_.failure) {
    return Result<void>::failure(extension_.failure);
  }
  auto graph_nodes = extension_.nodes;
  graph_nodes.push_back(source_);
  auto prepared = build(graph_, std::move(graph_nodes), extension_.connections, observation_);
  prepared_ = bool(prepared);
  return prepared;
}

SubscribePipeline::~SubscribePipeline() {
  (void)close();
  graph_.release();
}

Result<void> SubscribePipeline::start() {
  if (auto failure = observation_->failure()) {
    return Result<void>::failure(failure);
  }
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                                "Closed RTC pipeline cannot restart"));
    }
    if (running_) {
      return Result<void>::success();
    }
  }
  auto started = [&]() -> Result<void> {
    if (!prepared_) {
      if (auto prepared = prepare(); !prepared) {
        return prepared;
      }
    }
    if (!graph_.start()) {
      return Result<void>::failure(startup_failure(observation_, source_->track()));
    }
    if (extension_.activate) {
      detail::CallbackScope scope;
      if (auto activated = extension_.activate(); !activated) {
        return activated;
      }
    }
    if (auto activated = source_->activate(); !activated) {
      return activated;
    }
    std::lock_guard lock(mutex_);
    if (auto failure = observation_->failure()) {
      return Result<void>::failure(failure);
    }
    running_ = true;
    return Result<void>::success();
  }();
  if (!started) {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    const auto failure = started.failure_record();
    source_->quiesce();
    quiesce(extension_);
    graph_.stop();
    observation_->report(failure, {});
    notify_failure(failure);
    if (owns_endpoint_) {
      (void)source_->track()->close();
    }
  }
  return started;
}

Failure SubscribePipeline::failure() const {
  return observation_->failure();
}

void SubscribePipeline::notify_failure(Failure failure) {
  if (failure_notified_) {
    return;
  }
  failure_notified_ = true;
  if (extension_.on_failure) {
    detail::CallbackScope scope;
    extension_.on_failure(failure);
  }
}

ErrorCode SubscribePipeline::close() {
  if (!source_) {
    return ErrorCode::ok;
  }
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
  return stop && owns_endpoint_ ? source_->track()->close() : ErrorCode::ok;
}
}  // namespace livekit::sdk::rtc::engine
