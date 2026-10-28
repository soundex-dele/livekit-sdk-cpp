#include "engines/rtc/rtc_graph.hpp"
#include <algorithm>
#include <cmath>
namespace livekit::sdk::detail {
namespace { std::atomic<uint64_t> next_node_name{1}; }
MediaNode::MediaNode(std::string kind, NodeExecution execution, std::string format)
    : GraphNode(kind + "-" + std::to_string(next_node_name++), nullptr),
      kind_(std::move(kind)), format_(std::move(format)), execution_(execution) {}
NodeMetrics MediaNode::snapshot() const {
  std::lock_guard lock(format_mutex_);
  return {id_, kind_, format_, execution_, inputs.load(), outputs.load(), errors.load(), dropped.load(), generation.load(), getName(), track_id_};
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
void RtcGraph::bind_sync(const std::string& track,const std::string& group,const std::vector<uint32_t>& ssrcs){std::lock_guard lock(mutex_);sync_tracks_[track]={group,ssrcs};}
void RtcGraph::unbind_sync(const std::string& track){std::lock_guard lock(mutex_);auto it=sync_tracks_.find(track);if(it!=sync_tracks_.end()){audio_clocks_.erase(it->second.group);sync_tracks_.erase(it);}}
void RtcGraph::audio_playout(uint32_t ssrc,int64_t ntp_ms,int64_t local_us){
  if(!synchronize_av)return;std::lock_guard lock(mutex_);
  for(const auto& [track,info]:sync_tracks_)if(!info.group.empty()&&std::find(info.ssrcs.begin(),info.ssrcs.end(),ssrc)!=info.ssrcs.end()){
    auto& clock=audio_clocks_[info.group];clock={ntp_ms,local_us+audio_device_delay_us.load(),local_us};return;
  }
}
std::optional<int64_t> RtcGraph::video_playout(const std::string& track,int64_t ntp_ms,int64_t now_us){
  if(!synchronize_av||ntp_ms<=0)return {};std::lock_guard lock(mutex_);auto t=sync_tracks_.find(track);if(t==sync_tracks_.end())return {};
  auto c=audio_clocks_.find(t->second.group);if(c==audio_clocks_.end()||now_us-c->second.updated_us>1000000||std::abs(ntp_ms-c->second.ntp_ms)>2000)return {};
  return c->second.local_us+(ntp_ms-c->second.ntp_ms)*1000;
}
void RtcGraph::close() { std::lock_guard lock(mutex_); closing_ = true; video_routes_.clear();sync_tracks_.clear();audio_clocks_.clear(); }
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
