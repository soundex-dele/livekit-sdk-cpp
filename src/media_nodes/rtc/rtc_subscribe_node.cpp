#include "media_graph/failure.hpp"
#include "livekit/rtc/subscribe_node.hpp"

#include "backends/rtc/control_context.hpp"

namespace livekit::sdk::rtc::nodes {
RtcSubscribeNode::RtcSubscribeNode(std::string name, std::shared_ptr<SubscribedTrack> track)
    : GraphNode(name, nullptr), track_(std::move(track)) {
  if (!track_) {
    failure_ = make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                            "RtcSubscribeNode requires an endpoint");
  }
}

bool RtcSubscribeNode::doInit() {
  if (auto valid = status(); !valid) {
    reportError(livekit::sdk::detail::graph_error(valid.failure_record()));
    return false;
  }
  subscribe<KeyframeRequestEvent>([this](const auto&) {
    if (active_)
      track_->request_keyframe_async();
  });
  return true;
}

bool RtcSubscribeNode::doStart() {
  if (!status()) {
    return false;
  }
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

Result<void> RtcSubscribeNode::activate() {
  if (auto valid = status(); !valid) {
    return valid;
  }
  if (!active_) {
    return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                              "Activate requires a started RTC source"));
  }
  if (worker_.joinable()) {
    return Result<void>::success();
  }
  worker_ = std::jthread([this](std::stop_token stop) {
    detail::CallbackScope worker_scope;
    Failure failure;
    while (!stop.stop_requested()) {
      auto item = track_->wait_frame(stop);
      if (item.status == ReadStatus::failed) {
        const auto state = track_->snapshot();
        failure = state.failure
                      ? state.failure
                      : make_failure(ErrorDomain::media, ErrorCode::processing_failed, state.error);
        break;
      }
      if (item.status != ReadStatus::frame || stop.stop_requested()) {
        if (!stop.stop_requested() && terminal_) {
          terminal_();
        }
        break;
      }
      publish(EncodedFrameEvent(std::move(*item.frame)));
      if (auto reported = error()) {
        failure = livekit::sdk::detail::graph_failure(*reported);
        break;
      }
      ++delivered_;
    }
    if (failure) {
      failure = with_context(std::move(failure), {"receive", track_->descriptor().id, getName(),
                                                  track_->snapshot().epoch});
      {
        std::lock_guard lock(failure_mutex_);
        failure_ = failure;
      }
      reportError(livekit::sdk::detail::graph_error(failure));
    }
  });
  return Result<void>::success();
}

void RtcSubscribeNode::quiesce() {
  active_ = false;
  worker_.request_stop();
  if (worker_.joinable()) {
    worker_.join();
  }
}

Result<void> RtcSubscribeNode::status() const {
  std::lock_guard lock(failure_mutex_);
  if (failure_) {
    return Result<void>::failure(failure_);
  }
  return Result<void>::success();
}

void RtcSubscribeNode::doStop() {
  quiesce();
  // Endpoint teardown belongs to the serialized pipeline controller.
}
}  // namespace livekit::sdk::rtc::nodes
