#include "livekit/rtc/stream_pipeline.hpp"
#include <atomic>
#include <stdexcept>
#include <utility>

namespace livekit::sdk::rtc::engine {
namespace {
std::atomic<uint64_t> next_graph{0};

std::string name(const char* role, const TrackId& id) {
  return std::string("rtc.") + role + "." + std::to_string(++next_graph) + "." + id;
}

void build(graphflow::engine::GraphEngine& graph,
           std::vector<std::shared_ptr<graphflow::core::GraphNode>> nodes,
           std::vector<graphflow::engine::NodeConnection> connections) {
  if (!graph.loadFromNodes(nodes, connections) || !graph.init())
    throw std::runtime_error("Unable to prepare RTC track graph");
}

void quiesce(TrackGraphSpec& extension) {
  // Move first so partial activation and repeated close invoke this exactly once.
  if (auto stop = std::exchange(extension.quiesce, {}))
    stop();
}
}  // namespace

class PublishPipeline::Input final : public graphflow::core::GraphNode {
 public:
  explicit Input(const TrackId& id) : GraphNode(name("input", id), nullptr) {}

  WriteStatus push(EncodedFrame frame) {
    nodes::EncodedFrameEvent event(std::move(frame));
    event.receipt = std::make_shared<std::atomic<WriteStatus>>(WriteStatus::not_ready);
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

PublishPipeline::PublishPipeline(std::shared_ptr<PublishedTrack> track,
                                 PublishGraphFactory factory) {
  if (!track || !factory)
    throw std::invalid_argument("RTC publication requires endpoint and graph factory");
  input_ = std::make_shared<Input>(track->descriptor().id);
  sink_ = std::make_shared<nodes::RtcPublishNode>(name("publish", track->descriptor().id), track);
  extension_ = factory(sink_);
  auto graph_nodes = extension_.nodes;
  auto connections = extension_.connections;
  graph_nodes.insert(graph_nodes.begin(), sink_);
  graph_nodes.push_back(input_);
  connections.push_back(TrackGraphSpec::connect<nodes::EncodedFrameEvent>(input_, sink_));
  build(graph_, std::move(graph_nodes), std::move(connections));
}

PublishPipeline::~PublishPipeline() {
  close();
  graph_.release();
}

void PublishPipeline::start() {
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
    if (extension_.activate)
      extension_.activate();
    std::lock_guard lock(mutex_);
    running_ = true;
  } catch (...) {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    sink_->quiesce();
    quiesce(extension_);
    graph_.stop();
    sink_->track()->close();
    throw;
  }
}

WriteStatus PublishPipeline::write(EncodedFrame frame) {
  std::lock_guard lock(mutex_);
  if (closed_)
    return WriteStatus::closed;
  if (!running_)
    return WriteStatus::not_ready;
  return input_->push(std::move(frame));
}

void PublishPipeline::check() {
  {
    std::lock_guard lock(mutex_);
    if (!running_)
      return;
  }
  sink_->throw_if_failed();
  if (extension_.check_health) {
    extension_.check_health();
  }
}

std::future<void> PublishPipeline::close() {
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
  return sink_->track()->close();
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
                                     SubscribeGraphFactory factory) {
  if (!track || !factory)
    throw std::invalid_argument("RTC subscription requires endpoint and graph factory");
  source_ =
      std::make_shared<nodes::RtcSubscribeNode>(name("subscribe", track->descriptor().id), track);
  extension_ = factory(source_);
  auto graph_nodes = extension_.nodes;
  graph_nodes.push_back(source_);
  build(graph_, std::move(graph_nodes), extension_.connections);
}

SubscribePipeline::~SubscribePipeline() {
  close();
  graph_.release();
}

void SubscribePipeline::start() {
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
      extension_.activate();
    }
    source_->activate();
    std::lock_guard lock(mutex_);
    running_ = true;
  } catch (...) {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    source_->quiesce();
    quiesce(extension_);
    graph_.stop();
    source_->track()->close();
    throw;
  }
}

void SubscribePipeline::check() {
  {
    std::lock_guard lock(mutex_);
    if (!running_) {
      return;
    }
  }
  source_->throw_if_failed();
  if (extension_.check_health) {
    extension_.check_health();
  }
}

std::future<void> SubscribePipeline::close() {
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
  return source_->track()->close();
}
}  // namespace livekit::sdk::rtc::engine
