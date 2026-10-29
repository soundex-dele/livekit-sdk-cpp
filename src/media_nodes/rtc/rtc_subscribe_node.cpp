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
          const auto state = track_->snapshot();
          if (state.failure) {
            throw SDKError(state.failure);
          }
          throw std::runtime_error(state.error);
        }
        if (item.status != ReadStatus::frame || stop.stop_requested()) {
          if (!stop.stop_requested() && terminal_) {
            terminal_();
          }
          break;
        }
        publish(EncodedFrameEvent(std::move(*item.frame)));
        ++delivered_;
      }
    } catch (...) {
      const auto error = normalized_failure(
          std::current_exception(),
          {"receive", track_->descriptor().id, getName(), track_->snapshot().epoch},
          ErrorDomain::media, ErrorCode::processing_failed);
      {
        std::lock_guard lock(failure_mutex_);
        failure_ = error;
      }
      reportFailure(error);
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
