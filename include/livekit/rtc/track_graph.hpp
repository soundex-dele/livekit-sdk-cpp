#pragma once
#include "livekit/rtc/track_node.hpp"
#include <map>

namespace livekit::sdk::rtc {
// The engine retains processor handles for snapshots, but routes and lifecycle
// are exclusively owned by graphflow::Graph on its control thread.
class TrackBinding final : public graphflow::INode {
 public:
  explicit TrackBinding(std::shared_ptr<TrackNode> node) : node_(std::move(node)) {}

  void bind(graphflow::Binder& b) override {
    node_->bind(b);
  }

  graphflow::Status init() override {
    return status(node_->prepare());
  }

  graphflow::Status start() override {
    return status(node_->activate_graph());
  }

  graphflow::Status stop() override {
    node_->stop_graph();
    return {};
  }

  graphflow::Status release() override {
    node_->release_graph();
    return {};
  }

 private:
  graphflow::Status status(Result<void> r) {
    if (r) {
      return {};
    }
    node_->report_failure(r.failure_record());
    return {
        {graphflow::ErrorCode::NodeFailure, r.error().message, r.failure_record(), node_->name()}};
  }

  std::shared_ptr<TrackNode> node_;
};

struct TrackConnection {
  std::string from, to;
  using Ref = graphflow::NodeRef<TrackBinding>;
  using Wire = void (*)(graphflow::Wiring&, const Ref&, const Ref&, graphflow::RouteOptions);
  graphflow::RouteOptions options{graphflow::DeliveryPolicy::Reliable};

  template <class E>
  static TrackConnection between(std::string from, std::string to) {
    return {std::move(from), std::move(to), TrackPort<E>::out.name,
            [](graphflow::Wiring& w, const Ref& a, const Ref& b, graphflow::RouteOptions options) {
              w.connect(a.out(TrackPort<E>::out), b.in(TrackPort<E>::in), {}, options);
            }};
  }

  const char* type() const {
    return type_;
  }

  void connect(graphflow::Wiring& w, const Ref& a, const Ref& b) const {
    wire_(w, a, b, options);
  }

 private:
  TrackConnection(std::string a, std::string b, const char* type, Wire wire)
      : from(std::move(a)), to(std::move(b)), type_(type), wire_(wire) {}

  const char* type_;
  Wire wire_;
};

inline graphflow::Status wire_track_graph(graphflow::Graph& graph,
                                          const std::vector<std::shared_ptr<TrackNode>>& nodes,
                                          const std::vector<TrackConnection>& edges) {
  std::map<std::string, TrackConnection::Ref> refs;
  for (auto& node : nodes) {
    if (!node || !node->claim()) {
      return {{graphflow::ErrorCode::InvalidGraph, "track node already bound or null"}};
    }
    auto ref = graph.add<TrackBinding>(node->name(), node);
    if (!ref) {
      return ref.status();
    }
    refs.emplace(node->name(), *ref);
  }
  for (const auto& e : edges) {
    if (!refs.count(e.from) || !refs.count(e.to)) {
      return {{graphflow::ErrorCode::InvalidGraph, "unknown track endpoint"}};
    }
  }
  return graph.rewire([&](graphflow::Wiring& w) {
    for (auto& e : edges) {
      e.connect(w, refs.find(e.from)->second, refs.find(e.to)->second);
    }
  });
}
}  // namespace livekit::sdk::rtc
