#pragma once
#include "engine/graph_engine.h"

namespace livekit::sdk::test {
// Test graphs connect reusable media nodes directly, without a room engine.
template <class Event>
graphflow::engine::NodeConnection connect(const std::shared_ptr<graphflow::core::GraphNode>& from,
                                          const std::shared_ptr<graphflow::core::GraphNode>& to) {
  return {from->getName(), to->getName(), graphflow::core::detail::prettyName<Event>(), {}};
}
}  // namespace livekit::sdk::test
