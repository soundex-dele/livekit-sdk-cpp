#include "engines/rtc/rtc_graph.hpp"
namespace livekit::sdk::detail {
namespace { std::atomic<uint64_t> next_node_name{1}; }
MediaNode::MediaNode(std::string kind, NodeExecution execution, std::string format)
    : GraphNode(kind + "-" + std::to_string(next_node_name++), nullptr),
      kind_(std::move(kind)), format_(std::move(format)), execution_(execution) {}
NodeMetrics MediaNode::snapshot() const {
  std::lock_guard lock(format_mutex_);
  return {id_, kind_, format_, execution_, inputs.load(), outputs.load(), errors.load(), dropped.load(), generation.load(), getName()};
}
std::vector<NodeMetrics> RtcGraph::snapshot() {
  std::lock_guard lock(mutex_);
  std::vector<NodeMetrics> result;
  for (auto it = nodes_.begin(); it != nodes_.end();) {
    if (auto node = it->second.lock()) { result.push_back(node->snapshot()); ++it; }
    else it = nodes_.erase(it);
  }
  return result;
}
void RtcGraph::close() { std::lock_guard lock(mutex_); closing_ = true; video_routes_.clear(); }
void RtcGraph::bind_video(const std::vector<uint32_t>& ssrcs, const std::shared_ptr<VideoDecodeTarget>& route) {
  std::lock_guard lock(mutex_);
  if (closing_ || ssrcs.empty()) throw std::runtime_error("media_graph: missing receive SSRC or closed graph");
  for (auto ssrc : ssrcs) {
    auto it = video_routes_.find(ssrc);
    auto previous = it == video_routes_.end() ? nullptr : it->second.lock();
    if (!ssrc || (previous && previous != route)) throw std::runtime_error("media_graph: ambiguous receive SSRC");
  }
  for (auto ssrc : ssrcs) video_routes_[ssrc] = route;
}
void RtcGraph::unbind_video(const VideoDecodeTarget* route) {
  std::lock_guard lock(mutex_);
  for (auto it = video_routes_.begin(); it != video_routes_.end();) {
    auto current = it->second.lock();
    if (!current || current.get() == route) it = video_routes_.erase(it); else ++it;
  }
}
std::shared_ptr<VideoDecodeTarget> RtcGraph::video_route(uint32_t ssrc) {
  std::lock_guard lock(mutex_);
  auto it = video_routes_.find(ssrc);
  return closing_ || it == video_routes_.end() ? nullptr : it->second.lock();
}
void RtcGraph::observe(const std::vector<std::shared_ptr<MediaNode>>& nodes) {
  std::lock_guard lock(mutex_);
  if (closing_) throw std::runtime_error("media_graph: session is closing");
  for (auto it = nodes_.begin(); it != nodes_.end();)
    if (it->second.expired()) it = nodes_.erase(it); else ++it;
  for (const auto& node : nodes) {
    node->id_ = next_id_++;
    nodes_.emplace(node->id_, node);
  }
}
std::vector<RtcGraph::Connection> RtcGraph::connections() {
  std::lock_guard lock(mutex_);
  std::vector<Connection> result;
  for (const auto& [id, weak] : nodes_) if (auto node = weak.lock())
    for (const auto& edge : node->getPubEdges())
      result.push_back({edge.from(), edge.to(), edge.dataType()});
  return result;
}
void RtcPipeline::build(const std::vector<std::shared_ptr<MediaNode>>& nodes,
                        const std::vector<Connection>& connections) {
  std::vector<std::shared_ptr<graphflow::core::GraphNode>> graph_nodes(nodes.begin(), nodes.end());
  if (!engine_.loadFromNodes(graph_nodes, connections) || !engine_.init())
    throw std::runtime_error("media_graph: cannot build pipeline");
  registry_->observe(nodes);
}
}
