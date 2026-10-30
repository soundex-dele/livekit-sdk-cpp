#pragma once
#include <functional>
#include <memory>

namespace graphflow::core {
class GraphNode;
}

namespace livekit::sdk::rtc::nodes {
class RtcPublishNode;
class RtcSubscribeNode;
}  // namespace livekit::sdk::rtc::nodes

namespace livekit::sdk::rtc::engine {
struct TrackGraphSpec;
using PublishGraphFactory = std::function<TrackGraphSpec(std::shared_ptr<nodes::RtcPublishNode>)>;
using SubscribeGraphFactory =
    std::function<TrackGraphSpec(std::shared_ptr<nodes::RtcSubscribeNode>)>;
}  // namespace livekit::sdk::rtc::engine
