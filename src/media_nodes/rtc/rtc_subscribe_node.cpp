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

RtcSubscribeNode::~RtcSubscribeNode() {
  quiesce();
  release();
}

void RtcSubscribeNode::activate() {
  if (!active_) {
    throw std::logic_error("Activate requires a started RTC source");
  }
  if (worker_.joinable()) {
    return;
  }
  worker_ = std::jthread([this](std::stop_token stop) {
    try {
      while (!stop.stop_requested()) {
        auto item = track_->wait_frame(stop);
        if (item.status == ReadStatus::failed) {
          throw std::runtime_error(track_->snapshot().error);
        }
        if (item.status != ReadStatus::frame || stop.stop_requested()) {
          break;
        }
        publish(EncodedFrameEvent(std::move(*item.frame)));
        ++delivered_;
      }
    } catch (...) {
      std::lock_guard lock(failure_mutex_);
      failure_ = std::current_exception();
    }
  });
}

void RtcSubscribeNode::quiesce() {
  active_ = false;
  worker_.request_stop();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void RtcSubscribeNode::throw_if_failed() const {
  std::lock_guard lock(failure_mutex_);
  if (failure_) {
    std::rethrow_exception(failure_);
  }
}

void RtcSubscribeNode::doStop() {
  quiesce();
  track_->close();
}
}  // namespace livekit::sdk::rtc::nodes
