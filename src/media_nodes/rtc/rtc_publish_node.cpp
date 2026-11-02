#include <future>
#include "media_graph/failure.hpp"
#include "livekit/rtc/publish_node.hpp"

#include "backends/rtc/control_context.hpp"

namespace livekit::sdk::rtc::nodes {
RtcPublishNode::RtcPublishNode(std::string name, std::shared_ptr<PublishedTrack> track)
    : GraphNode(name, nullptr), track_(std::move(track)) {
  if (!track_) {
    failure_ = make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                            "RtcPublishNode requires an endpoint");
  }
}

bool RtcPublishNode::doInit() {
  if (auto valid = status(); !valid) {
    reportError(livekit::sdk::detail::graph_error(valid.failure_record()));
    return false;
  }
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
  if (!status()) {
    return false;
  }
  const auto state = track_->snapshot().state;
  if (state == TrackState::closed || state == TrackState::failed)
    return false;
  active_ = true;
  return true;
}

RtcPublishNode::~RtcPublishNode() {
  quiesce();
  release();
}

Result<void> RtcPublishNode::activate() {
  if (auto valid = status(); !valid) {
    return valid;
  }
  if (!active_) {
    return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                              "Activate requires a started RTC sink"));
  }
  if (worker_.joinable()) {
    return Result<void>::success();
  }
  std::promise<Result<void>> initial;
  auto ready = initial.get_future();
  worker_ = std::jthread([this, initial = std::move(initial)](std::stop_token stop) mutable {
    detail::CallbackScope worker_scope;
    bool initialized = false;
    Epoch epoch = 0;
    uint64_t revision = UINT64_MAX;
    Failure failure;
    while (!stop.stop_requested()) {
      auto feedback = track_->wait_feedback(epoch, revision, stop);
      if (!feedback) {
        if (stop.stop_requested()) {
          break;
        }
        const auto state = track_->snapshot();
        if (state.state == TrackState::failed) {
          failure = state.failure ? state.failure
                                  : make_failure(ErrorDomain::media, ErrorCode::processing_failed,
                                                 state.error);
        } else if (!initialized) {
          failure = make_failure(ErrorDomain::media, ErrorCode::session_closed,
                                 "RTC publication closed before initial feedback");
        } else if (terminal_) {
          terminal_();
        }
        break;
      }
      epoch = feedback->epoch;
      revision = feedback->revision;
      publish(SenderFeedbackEvent(std::move(*feedback)));
      if (auto reported = error()) {
        failure = livekit::sdk::detail::graph_failure(*reported);
        break;
      }
      if (!initialized) {
        initial.set_value(Result<void>::success());
        initialized = true;
      }
    }
    if (!initialized && !failure) {
      failure = make_failure(ErrorDomain::media, ErrorCode::cancelled,
                             "RTC publication activation cancelled");
    }
    if (failure) {
      failure = with_context(std::move(failure), {"feedback", track_->descriptor().id, getName(),
                                                  track_->snapshot().epoch});
      {
        std::lock_guard lock(failure_mutex_);
        failure_ = failure;
      }
      reportError(livekit::sdk::detail::graph_error(failure));
      if (!initialized) {
        initial.set_value(Result<void>::failure(failure));
      }
    }
  });
  return ready.get();
}

void RtcPublishNode::quiesce() {
  active_ = false;
  worker_.request_stop();
  if (worker_.joinable()) {
    worker_.join();
  }
}

Result<void> RtcPublishNode::status() const {
  std::lock_guard lock(failure_mutex_);
  if (failure_) {
    return Result<void>::failure(failure_);
  }
  return Result<void>::success();
}

void RtcPublishNode::doStop() {
  quiesce();
  // Local graph stop never waits for native signaling.
  // Endpoint teardown belongs to the serialized pipeline controller.
}
}  // namespace livekit::sdk::rtc::nodes
