#pragma once
#include "core/graph_node.h"
#include "livekit/media_core/frame.hpp"
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <unordered_map>

namespace livekit::sdk::detail {
// Dispatch stays on the playback worker. A receiver explicitly acknowledges an
// event only after taking ownership; rejection leaves it with the publisher.
struct MediaDelivery {
  bool accepted = false;
  std::exception_ptr error;
};
template<class Derived> struct MediaEvent : graphflow::core::EventT<Derived> {
  uint64_t epoch = 0;
  bool eos = false;
  std::shared_ptr<MediaDelivery> delivery;
};
struct RGBA8 : MediaEvent<RGBA8> { media::VideoFrame frame; };
struct StereoFloat48k : MediaEvent<StereoFloat48k> { media::AudioFrame frame; };

class MediaNode : public graphflow::core::GraphNode {
 public:
  using GraphNode::GraphNode;
  // Called after every node has installed its subscriptions during init().
  virtual void prepare() {}
  // One bounded unit of work. The executor knows no concrete media node types.
  virtual bool pump() { return false; }
  bool finished() const { return finished_; }
  uint64_t received() const { return received_; }
  uint64_t published() const { return published_; }
  void reset(int64_t position) { ++epoch_; finished_ = false; on_reset(position); }
 protected:
  virtual void on_reset(int64_t) {}
  template<class E, class F> void subscribe_media(F callback) {
    const auto type = E{}.typeName();
    if (getSubEdges().size() != 1 || !getSubEdges().front().eventBus() ||
        getSubEdges().front().dataType() != type)
      throw std::logic_error("media/subscribe: incompatible input for " + getName());
    subscribe<E>([this, callback = std::move(callback)](E& event) {
      if (!event.delivery) return;
      try {
        if (state() == graphflow::core::LifecycleState::Released)
          throw std::logic_error("media/receive: released node " + getName());
        if (event.epoch != epoch_) {
          // Old in-flight data is discarded; a future epoch is a wiring error.
          if (event.epoch > epoch_) throw std::logic_error("media/receive: future epoch");
          event.delivery->accepted = true;
        } else if (callback(event)) {
          event.delivery->accepted = true;
          ++received_;
        }
      } catch (...) { event.delivery->error = std::current_exception(); }
    });
  }
  template<class E> bool publish_media(E event, const std::string& route = "") {
    // One subscriber per output port: retries must never redeliver to a branch
    // that already accepted. Fan-out requires an explicit buffering node.
    const graphflow::core::GraphEdge* selected = nullptr;
    for (const auto& edge : getPubEdges()) if (edge.condition() == route) {
      if (selected) throw std::logic_error("media/publish: ambiguous output port");
      selected = &edge;
    }
    if (!selected || !selected->eventBus() || selected->dataType() != event.typeName() ||
        selected->eventBus()->handlerCount(selected->name()) != 1)
      throw std::logic_error("media/publish: missing or incompatible subscriber for " + getName());
    event.epoch = epoch_;
    event.delivery = std::make_shared<MediaDelivery>();
    publish(event, route);
    if (event.delivery->error) std::rethrow_exception(event.delivery->error);
    if (event.delivery->accepted) ++published_;
    return event.delivery->accepted;
  }
  uint64_t epoch_ = 0;
  bool finished_ = false;
 private:
  uint64_t received_ = 0, published_ = 0;
};

// Build execution order from actual GraphBuilder connections, not a hardcoded
// decoder/converter call chain. Downstream first frees capacity before producing.
class MediaExecutor {
 public:
  enum class Step { progress, backpressured, end };
  explicit MediaExecutor(const std::vector<std::shared_ptr<MediaNode>>& nodes) {
    std::unordered_map<std::string, size_t> indexes;
    std::vector<size_t> incoming(nodes.size());
    std::vector<std::vector<size_t>> outgoing(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
      if (!indexes.emplace(nodes[i]->getName(), i).second) throw std::logic_error("media/graph: duplicate node");
    for (size_t i = 0; i < nodes.size(); ++i) for (const auto& edge : nodes[i]->getPubEdges()) {
      const auto target = indexes.find(edge.to());
      if (edge.from() != nodes[i]->getName() || target == indexes.end() || !edge.eventBus())
        throw std::logic_error("media/graph: invalid connection");
      outgoing[i].push_back(target->second); ++incoming[target->second];
    }
    std::vector<size_t> ready;
    for (size_t i = 0; i < nodes.size(); ++i) if (!incoming[i]) ready.push_back(i);
    for (size_t i = 0; i < ready.size(); ++i) {
      const auto index = ready[i]; ordered_.push_back(nodes[index]);
      for (auto next : outgoing[index]) if (--incoming[next] == 0) ready.push_back(next);
    }
    if (ordered_.size() != nodes.size()) throw std::logic_error("media/graph: cycle");
  }
  const auto& nodes() const { return ordered_; }
  Step step() {
    bool progress = false;
    for (auto it = ordered_.rbegin(); it != ordered_.rend(); ++it) {
      if ((*it)->state() != graphflow::core::LifecycleState::Running)
        throw std::logic_error("media/step: node is not running");
      progress = (*it)->pump() || progress;
    }
    if (std::all_of(ordered_.begin(), ordered_.end(), [](const auto& node) { return node->finished(); })) return Step::end;
    return progress ? Step::progress : Step::backpressured;
  }
  void reset(int64_t position) { for (const auto& node : ordered_) node->reset(position); }
 private:
  std::vector<std::shared_ptr<MediaNode>> ordered_;
};
}
