#pragma once
#include "core/graph_node.h"
#include "engine/graph_engine.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace livekit::sdk::detail {
enum class NodeExecution { source, backend, sink };
struct NodeMetrics {
  uint64_t id = 0;
  std::string kind, format;
  NodeExecution execution = NodeExecution::backend;
  uint64_t inputs = 0, outputs = 0, errors = 0, dropped = 0, generation = 0;
  std::string name;
};

// Lifecycle is owned by the node's creating/releasing sequence. Media methods
// follow the backend's sequence contract; counters are atomic and format is locked.
class MediaNode : public graphflow::core::GraphNode {
 public:
  MediaNode(std::string kind, NodeExecution execution, std::string format = {});
  ~MediaNode() override = default;
  NodeMetrics snapshot() const;
  std::atomic<uint64_t> inputs{0}, outputs{0}, errors{0}, dropped{0}, generation{0};
 protected:
  void set_format(std::string format) { std::lock_guard lock(format_mutex_); format_ = std::move(format); }
 private:
  friend class RtcGraph;
  uint64_t id_ = 0;
  std::string kind_, format_;
  NodeExecution execution_;
  mutable std::mutex format_mutex_;
};

// Registry of the actual media graph, not a speculative track->codec
// map. Weak ownership lets WebRTC release/recreate codecs without retaining an
// unbounded history. The session owns this registry; adapters lease their nodes.
// Video encoding runs on graph-owned EncodeExecutor sequences. Audio and
// receive-side codecs preserve WebRTC's original scheduling.
class RtcGraph {
 public:
  // Engine-owned nodes are registered only after their topology is immutable.
  void observe(const std::vector<std::shared_ptr<MediaNode>>& nodes);
  struct Connection { std::string from, to, data_type; };
  std::vector<Connection> connections();
  template<class T> std::shared_ptr<T> add(std::shared_ptr<T> node) {
    std::lock_guard lock(mutex_);
    if (closing_) throw std::runtime_error("media_graph: session is closing");
    for (auto it = nodes_.begin(); it != nodes_.end();)
      if (it->second.expired()) it = nodes_.erase(it); else ++it;
    auto& media_node = static_cast<MediaNode&>(*node);
    media_node.id_ = next_id_++;
    if (!node->init()) throw std::runtime_error("media_graph: node prepare failed");
    nodes_.emplace(media_node.id_, node);
    return node;
  }
  std::vector<NodeMetrics> snapshot();
  void close(); // closes admission; PC/source owners quiesce before releasing nodes
 private:
  std::mutex mutex_;
  bool closing_ = false;
  uint64_t next_id_ = 1;
  std::map<uint64_t, std::weak_ptr<MediaNode>> nodes_;
};

// One immutable topology per track/codec instance. All lifecycle operations run
// on its owner sequence. Producers must quiesce before release destroys buses.
class RtcPipeline {
 public:
  using Connection = graphflow::engine::NodeConnection;
  explicit RtcPipeline(std::shared_ptr<RtcGraph> registry) : registry_(std::move(registry)) {}
  ~RtcPipeline() { engine_.release(); }
  void build(const std::vector<std::shared_ptr<MediaNode>>& nodes,
             const std::vector<Connection>& connections);
  bool start() { return engine_.start(); }
  void stop() { engine_.stop(); }
  void release() { engine_.release(); }
  template<class Event> static Connection connect(const std::shared_ptr<MediaNode>& from,
                                                  const std::shared_ptr<MediaNode>& to) {
    return {from->getName(), to->getName(), graphflow::core::detail::prettyName<Event>(), {}};
  }
 private:
  std::shared_ptr<RtcGraph> registry_;
  graphflow::engine::GraphEngine engine_{1};
};

// Audio adapters retain their codec and factory lifetime together, including
// leases held by NetEq's parsed frames. No WebRTC types escape public SDK headers.
template<class Backend> class CodecNode final : public MediaNode {
 public:
  CodecNode(std::string kind, std::string format, std::shared_ptr<void> owner,
            std::unique_ptr<Backend> backend)
      : MediaNode(std::move(kind), NodeExecution::backend, std::move(format)),
        owner_(std::move(owner)), backend_(std::move(backend)) {}
  ~CodecNode() override { backend_.reset(); release(); }
  Backend& backend() { return *backend_; }
  template<class F> decltype(auto) process(F&& work) {
    inputs.fetch_add(1, std::memory_order_relaxed);
    return std::forward<F>(work)(*backend_);
  }
 private:
  std::shared_ptr<void> owner_;
  std::unique_ptr<Backend> backend_;
};
}
