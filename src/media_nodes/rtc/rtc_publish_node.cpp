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

RtcPublishNode::~RtcPublishNode() {
  quiesce();
  release();
}

void RtcPublishNode::activate() {
  if (!active_) {
    throw std::logic_error("Activate requires a started RTC sink");
  }
  if (worker_.joinable()) {
    return;
  }
  std::promise<void> initial;
  auto ready = initial.get_future();
  worker_ = std::jthread([this, initial = std::move(initial)](std::stop_token stop) mutable {
    bool initialized = false;
    try {
      Epoch epoch = 0;
      uint64_t revision = UINT64_MAX;
      while (!stop.stop_requested()) {
        auto feedback = track_->wait_feedback(epoch, revision, stop);
        if (!feedback) {
          if (!initialized) {
            throw std::runtime_error("RTC publication closed before initial feedback");
          }
          break;
        }
        epoch = feedback->epoch;
        revision = feedback->revision;
        publish(SenderFeedbackEvent(std::move(*feedback)));
        if (!initialized) {
          initial.set_value();
          initialized = true;
        }
      }
    } catch (...) {
      const auto failure = std::current_exception();
      {
        std::lock_guard lock(failure_mutex_);
        failure_ = failure;
      }
      if (!initialized) {
        initial.set_exception(failure);
      }
    }
  });
  ready.get();
}

void RtcPublishNode::quiesce() {
  active_ = false;
  worker_.request_stop();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void RtcPublishNode::throw_if_failed() const {
  std::lock_guard lock(failure_mutex_);
  if (failure_) {
    std::rethrow_exception(failure_);
  }
}

void RtcPublishNode::doStop() {
  quiesce();
  // Local graph stop never waits for native signaling.
  track_->close();
}
}  // namespace livekit::sdk::rtc::nodes
