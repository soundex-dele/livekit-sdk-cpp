#include "livekit/rtc/subscribe_node.hpp"
#include <stdexcept>

namespace livekit::sdk::rtc::nodes {
RtcSubscribeNode::RtcSubscribeNode(std::string name, std::shared_ptr<SubscribedTrack> track)
    : GraphNode(name, nullptr), track_(std::move(track)) {
  if (!track_)
    throw std::invalid_argument("RtcSubscribeNode requires a subscription endpoint");
}

bool RtcSubscribeNode::doInit() {
  subscribe<KeyframeRequestEvent>([this](const auto&) {
    if (active_)
      track_->request_keyframe();
  });
  return true;
}

bool RtcSubscribeNode::doStart() {
  const auto state = track_->snapshot().state;
  if (state == TrackState::closed || state == TrackState::failed)
    return false;
  active_ = true;
  return true;
}

void RtcSubscribeNode::doStop() {
  active_ = false;
  track_->close();
}

size_t RtcSubscribeNode::poll(size_t max_frames) {
  if (!active_)
    return 0;
  if (!max_frames || max_frames > 64)
    throw std::invalid_argument("Invalid RTC graph poll budget");
  size_t count = 0;
  while (count < max_frames) {
    auto item = track_->read(std::chrono::milliseconds::zero());
    if (item.status != ReadStatus::frame)
      break;
    publish(EncodedFrameEvent(std::move(*item.frame)));
    ++count;
    ++delivered_;
  }
  return count;
}
}  // namespace livekit::sdk::rtc::nodes
