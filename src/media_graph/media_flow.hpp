#pragma once
#include "core/graph_node.h"
#include "livekit/media_core/frame.hpp"
#include <algorithm>
#include "livekit/result.hpp"
#include <unordered_map>

namespace livekit::sdk::detail {
// Dispatch stays on the playback worker. A receiver explicitly acknowledges an
// event only after taking ownership; rejection leaves it with the publisher.
struct MediaDelivery {
  bool accepted = false;
  Failure error;
};

template <class Derived>
struct MediaEvent : graphflow::core::EventT<Derived> {
  uint64_t epoch = 0;
  bool eos = false;
  std::shared_ptr<MediaDelivery> delivery;
};

struct RGBA8 : MediaEvent<RGBA8> {
  media::VideoFrame frame;
};

struct StereoFloat48k : MediaEvent<StereoFloat48k> {
  media::AudioFrame frame;
};

class MediaNode : public graphflow::core::GraphNode {
 public:
  using GraphNode::GraphNode;

  // Called after every node has installed its subscriptions during init().
  virtual Result<void> prepare() {
    return Result<void>::success();
  }

  // One bounded unit of work. The executor knows no concrete media node types.
  virtual Result<bool> pump() {
    return Result<bool>::success(false);
  }

  bool finished() const {
    return finished_;
  }

  uint64_t received() const {
    return received_;
  }

  uint64_t published() const {
    return published_;
  }

  Result<void> reset(int64_t position) {
    ++epoch_;
    finished_ = false;
    return on_reset(position);
  }

 protected:
  virtual Result<void> on_reset(int64_t) {
    return Result<void>::success();
  }

  template <class E, class F>
  Result<void> subscribe_media(F callback) {
    const auto type = E{}.typeName();
    if (getSubEdges().size() != 1 || !getSubEdges().front().eventBus() ||
        getSubEdges().front().dataType() != type) {
      return Result<void>::failure(
          make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                       "media/subscribe: incompatible input for " + getName(), false, 0,
                       {"subscribe_media", {}, getName()}));
    }
    subscribe<E>([this, callback = std::move(callback)](E& event) {
      if (!event.delivery) {
        return;
      }
      if (state() == graphflow::core::LifecycleState::Released) {
        event.delivery->error = make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                             "media/receive: released node " + getName(), false, 0,
                                             {"receive_media", {}, getName()});
        return;
      }
      if (event.epoch != epoch_) {
        if (event.epoch > epoch_) {
          event.delivery->error = make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                               "media/receive: future epoch", false, 0,
                                               {"receive_media", {}, getName()});
        } else {
          event.delivery->accepted = true;
        }
        return;
      }
      auto received = callback(event);
      if (!received) {
        event.delivery->error = received.failure_record();
      } else if (*received) {
        event.delivery->accepted = true;
        ++received_;
      }
    });
    return Result<void>::success();
  }

  template <class E>
  Result<bool> publish_media(E event, const std::string& route = "") {
    const graphflow::core::GraphEdge* selected = nullptr;
    for (const auto& edge : getPubEdges()) {
      if (edge.condition() == route) {
        if (selected) {
          return Result<bool>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                                    "media/publish: ambiguous output port", false,
                                                    0, {"publish_media", {}, getName()}));
        }
        selected = &edge;
      }
    }
    if (!selected || !selected->eventBus() || selected->dataType() != event.typeName() ||
        selected->eventBus()->handlerCount(selected->name()) != 1) {
      return Result<bool>::failure(
          make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                       "media/publish: missing or incompatible subscriber for " + getName(), false,
                       0, {"publish_media", {}, getName()}));
    }
    event.epoch = epoch_;
    event.delivery = std::make_shared<MediaDelivery>();
    publish(event, route);
    if (event.delivery->error) {
      return Result<bool>::failure(event.delivery->error);
    }
    if (event.delivery->accepted) {
      ++published_;
    }
    return Result<bool>::success(event.delivery->accepted);
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
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (!nodes[i] || !indexes.emplace(nodes[i]->getName(), i).second) {
        failure_ = make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                "media/graph: null or duplicate node");
        return;
      }
    }
    for (size_t i = 0; i < nodes.size(); ++i)
      for (const auto& edge : nodes[i]->getPubEdges()) {
        const auto target = indexes.find(edge.to());
        if (edge.from() != nodes[i]->getName() || target == indexes.end() || !edge.eventBus()) {
          failure_ = make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                  "media/graph: invalid connection");
          return;
        }
        outgoing[i].push_back(target->second);
        ++incoming[target->second];
      }
    std::vector<size_t> ready;
    for (size_t i = 0; i < nodes.size(); ++i)
      if (!incoming[i])
        ready.push_back(i);
    for (size_t i = 0; i < ready.size(); ++i) {
      const auto index = ready[i];
      ordered_.push_back(nodes[index]);
      for (auto next : outgoing[index])
        if (--incoming[next] == 0)
          ready.push_back(next);
    }
    if (ordered_.size() != nodes.size()) {
      failure_ =
          make_failure(ErrorDomain::media, ErrorCode::invalid_argument, "media/graph: cycle");
      ordered_.clear();
    }
  }

  const auto& nodes() const {
    return ordered_;
  }

  Result<void> status() const {
    return failure_ ? Result<void>::failure(failure_) : Result<void>::success();
  }

  Result<Step> step() {
    if (failure_) {
      return Result<Step>::failure(failure_);
    }
    bool progress = false;
    for (auto it = ordered_.rbegin(); it != ordered_.rend(); ++it) {
      if ((*it)->state() != graphflow::core::LifecycleState::Running) {
        return Result<Step>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                                  "media/step: node is not running"));
      }
      auto pumped = (*it)->pump();
      if (!pumped) {
        return Result<Step>::failure(pumped.failure_record());
      }
      progress = *pumped || progress;
    }
    if (std::all_of(ordered_.begin(), ordered_.end(),
                    [](const auto& node) { return node->finished(); })) {
      return Result<Step>::success(Step::end);
    }
    return Result<Step>::success(progress ? Step::progress : Step::backpressured);
  }

  Result<void> reset(int64_t position) {
    if (failure_) {
      return Result<void>::failure(failure_);
    }
    for (const auto& node : ordered_) {
      if (auto result = node->reset(position); !result) {
        failure_ = result.failure_record();
        return result;
      }
    }
    return Result<void>::success();
  }

 private:
  std::vector<std::shared_ptr<MediaNode>> ordered_;
  Failure failure_;
};
}  // namespace livekit::sdk::detail
