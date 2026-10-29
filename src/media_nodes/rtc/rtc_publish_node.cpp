#include "livekit/rtc/publish_node.hpp"
#include <stdexcept>

namespace livekit::sdk::rtc::nodes {
RtcPublishNode::RtcPublishNode(std::string name, std::shared_ptr<PublishedTrack> track)
    : GraphNode(name, nullptr), track_(std::move(track)) {
  if (!track_)
    throw std::invalid_argument("RtcPublishNode requires a publication endpoint");
}

bool RtcPublishNode::doInit() {
  subscribe<EncodedFrameEvent>([this](const EncodedFrameEvent& event) {
    const auto result = active_.load() ? track_->try_write(event.frame) : WriteStatus::not_ready;
    last_write_ = result;
    if (event.receipt)
      *event.receipt = result;
    if (result == WriteStatus::accepted)
      ++accepted_;
    else
      ++rejected_;
  });
  return true;
}

bool RtcPublishNode::doStart() {
  const auto state = track_->snapshot().state;
  if (state == TrackState::closed || state == TrackState::failed)
    return false;
  active_ = true;
  return true;
}

void RtcPublishNode::doStop() {
  active_ = false;
  // Closing a track is asynchronous. Engine teardown awaits its own endpoint
  // close barrier; GraphEngine lifecycle methods never wait on native signaling.
  track_->close();
}

void RtcPublishNode::poll_feedback() {
  if (!active_)
    return;
  auto feedback = track_->feedback();
  if (feedback.epoch == feedback_epoch_ && feedback.revision == feedback_revision_)
    return;
  feedback_epoch_ = feedback.epoch;
  feedback_revision_ = feedback.revision;
  publish(SenderFeedbackEvent(std::move(feedback)));
}
}  // namespace livekit::sdk::rtc::nodes
