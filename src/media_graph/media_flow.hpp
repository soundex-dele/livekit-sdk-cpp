#pragma once
#include <graphflow/graph.hpp>
#include "livekit/media_core/frame.hpp"
#include <algorithm>
#include <chrono>
#include <functional>
#include "livekit/result.hpp"
#include <unordered_map>
#include <set>

namespace livekit::sdk::detail {
// Dispatch stays on the playback worker. A receiver explicitly acknowledges an
// event only after taking ownership; rejection leaves it with the publisher.
struct MediaDelivery {
  bool accepted = false;
  Failure error;
};

struct MediaEvent {
  uint64_t epoch = 0;
  bool eos = false;
  std::shared_ptr<MediaDelivery> delivery;
};

struct RGBA8 : MediaEvent {
  static constexpr const char* name = "RGBA8";
  media::VideoFrame frame;
};

struct StereoFloat48k : MediaEvent {
  static constexpr const char* name = "StereoFloat48k";
  media::AudioFrame frame;
};

enum class MediaState { Created, Initialized, Running, Stopped, Released };

inline Failure media_graph_failure(const graphflow::Status& status) {
  if (status.error.detail) {
    return std::static_pointer_cast<const FailureInfo>(status.error.detail);
  }
  return make_failure(ErrorDomain::media, ErrorCode::processing_failed, status.error.message);
}

class MediaNode {
 public:
  explicit MediaNode(std::string name) : name_(std::move(name)) {}

  virtual ~MediaNode() = default;

  const std::string& getName() const {
    return name_;
  }

  MediaState state() const {
    return state_;
  }

  // Called after all init hooks succeeded and graph-owned output ports opened.
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

  virtual void bind(graphflow::Binder&) = 0;

  virtual Result<void> doInit() {
    return Result<void>::success();
  }

  virtual void doRelease() {}

  template <class E, class F>
  void subscribe_media(graphflow::Binder& binder, F callback) {
    binder.input(
        graphflow::In<E>{"in", graphflow::Connections::ExactlyOne},
        [this, callback = std::move(callback)](const E& event) {
          if (!event.delivery) {
            return;
          }
          if (state() == MediaState::Released) {
            event.delivery->error = make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                                 "media/receive: released node " + getName(), false,
                                                 0, {"receive_media", {}, getName()});
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
  }

  template <class E>
  Result<bool> publish_media(const graphflow::Emitter<E>& output, E event) {
    event.epoch = epoch_;
    event.delivery = std::make_shared<MediaDelivery>();
    auto sent = output.emit(event);
    if (sent.status == graphflow::Admission::Failed) {
      return Result<bool>::failure(make_failure(ErrorDomain::media, ErrorCode::processing_failed,
                                                "media/publish: " + getName() + " -> " +
                                                    sent.error.node + "." + sent.error.port + ": " +
                                                    sent.error.message));
    }
    if (sent.status != graphflow::Admission::Accepted || sent.accepted != 1) {
      return Result<bool>::failure(
          make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                       "media/publish: inactive or unwired port on " + getName()));
    }
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
  friend class MediaStage;
  std::string name_;
  MediaState state_ = MediaState::Created;
  uint64_t received_ = 0, published_ = 0;
};

// Graph owns the lifecycle adapter; the pump and delivery worker can retain the
// component (not a mutable Graph node handle), including a released output queue.
class MediaStage : public graphflow::INode {
 public:
  explicit MediaStage(std::shared_ptr<MediaNode> node) : node_(std::move(node)) {}

  void bind(graphflow::Binder& binder) override {
    node_->bind(binder);
  }

  graphflow::Status init() override {
    auto result = node_->doInit();
    if (!result) {
      (void)release();
      return {{graphflow::ErrorCode::NodeFailure, result.error().message, result.failure_record()}};
    }
    node_->state_ = MediaState::Initialized;
    return {};
  }

  graphflow::Status start() override {
    node_->state_ = MediaState::Running;
    return {};
  }

  graphflow::Status stop() override {
    node_->state_ = MediaState::Stopped;
    return {};
  }

  graphflow::Status release() override {
    node_->doRelease();
    node_->state_ = MediaState::Released;
    return {};
  }

  ~MediaStage() override {
    if (node_->state_ != MediaState::Released) {
      (void)release();
    }
  }

 private:
  std::shared_ptr<MediaNode> node_;
};

struct MediaBinding {
  MediaBinding(graphflow::Graph& graph, std::shared_ptr<MediaNode> n)
      : node(std::move(n)), ref(graph.add<MediaStage>(node->getName(), node)) {}

  std::shared_ptr<MediaNode> node;
  graphflow::Result<graphflow::NodeRef<MediaStage>> ref;
};

struct MediaEdge {
  std::string from, to, type, port;
};

// The same typed connect call installs the route and records its pump dependency.
// The caller publishes the candidate diagnostics only after rewire succeeds.
template <class E>
void connect_media(graphflow::Wiring& w, std::vector<MediaEdge>& edges, const MediaBinding& source,
                   const MediaBinding& target, const char* port = "out") {
  w.connect(source.ref->out(graphflow::Out<E>{port}), target.ref->in(graphflow::In<E>{"in"}));
  edges.push_back({source.node->getName(), target.node->getName(), E::name, port});
}

class MediaExecutor {
 public:
  enum class Step { progress, backpressured, end };

  MediaExecutor(const std::vector<std::shared_ptr<MediaNode>>& nodes,
                const std::vector<MediaEdge>& edges) {
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
    std::set<std::pair<std::string, std::string>> outputs;
    for (const auto& edge : edges) {
      auto from = indexes.find(edge.from), to = indexes.find(edge.to);
      if (from == indexes.end() || to == indexes.end() ||
          !outputs.emplace(edge.from, edge.port).second) {
        failure_ = make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                "media/graph: invalid or ambiguous connection");
        return;
      }
      outgoing[from->second].push_back(to->second);
      ++incoming[to->second];
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

  // Optional, worker-thread observation of each bounded pump (including nested
  // inline sends). Ordinary playback does not read a clock for this probe.
  using PumpProbe =
      std::function<void(const std::string&, std::chrono::steady_clock::duration, bool)>;

  Result<Step> step(const PumpProbe& probe = {}) {
    if (failure_) {
      return Result<Step>::failure(failure_);
    }
    bool progress = false;
    for (auto it = ordered_.rbegin(); it != ordered_.rend(); ++it) {
      if ((*it)->state() != MediaState::Running) {
        return Result<Step>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                                  "media/step: node is not running"));
      }
      const auto begin =
          probe ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      auto pumped = (*it)->pump();
      if (probe)
        probe((*it)->getName(), std::chrono::steady_clock::now() - begin, pumped && *pumped);
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
