#include "livekit/rtc/track_endpoint.hpp"
#include "backends/rtc/endpoint.hpp"
#include "backends/rtc/track_control.hpp"

namespace livekit::sdk::rtc {
bool is_callback_thread() noexcept {
  return detail::callback_depth != 0;
}

PublishedTrack::PublishedTrack(std::shared_ptr<detail::Endpoint> endpoint,
                               std::weak_ptr<detail::TrackControl> session)
    : endpoint_(std::move(endpoint)), session_(std::move(session)) {}

const TrackDescriptor& PublishedTrack::descriptor() const {
  return endpoint_->descriptor;
}

TrackSnapshot PublishedTrack::snapshot() const {
  return endpoint_->snapshot();
}

SenderFeedback PublishedTrack::feedback() const {
  return endpoint_->feedback();
}

std::optional<SenderFeedback> PublishedTrack::wait_feedback(Epoch epoch, uint64_t revision,
                                                            std::stop_token stop) const {
  return endpoint_->wait_feedback(epoch, revision, stop);
}

WriteStatus PublishedTrack::try_write(EncodedFrame frame) {
  const auto result = endpoint_->write(std::move(frame));
  if (result == WriteStatus::accepted) {
    if (auto session = session_.lock()) {
      session->send(endpoint_);
    }
  }
  return result;
}

ErrorCode PublishedTrack::set_muted(bool muted, ControlOptions) {
  if (auto session = session_.lock()) {
    session->set_muted(descriptor().id, muted);
    return ErrorCode::ok;
  }
  return ErrorCode::session_closed;
}

ErrorCode PublishedTrack::close() {
  if (auto session = session_.lock()) {
    session->close_track(endpoint_);
    return ErrorCode::ok;
  }
  return ErrorCode::session_closed;
}

ErrorCode PublishedTrack::close_locked() {
  return close();
}

SubscribedTrack::SubscribedTrack(std::shared_ptr<detail::Endpoint> endpoint,
                                 std::weak_ptr<detail::TrackControl> session)
    : endpoint_(std::move(endpoint)), session_(std::move(session)) {}

const TrackDescriptor& SubscribedTrack::descriptor() const {
  return endpoint_->descriptor;
}

TrackSnapshot SubscribedTrack::snapshot() const {
  return endpoint_->snapshot();
}

ReadResult SubscribedTrack::read(std::chrono::milliseconds timeout, std::stop_token stop) {
  return endpoint_->read(timeout, stop);
}

ReadResult SubscribedTrack::wait_frame(std::stop_token stop) {
  return endpoint_->wait_frame(stop);
}

ErrorCode SubscribedTrack::request_keyframe() {
  endpoint_->require_keyframe();
  if (auto session = session_.lock()) {
    session->request_keyframe(endpoint_);
  }
  return ErrorCode::ok;
}

ErrorCode SubscribedTrack::set_demand(ReceiveDemand demand, ControlOptions) {
  if (auto session = session_.lock()) {
    session->set_receive_demand(descriptor().id, demand);
    return ErrorCode::ok;
  }
  return ErrorCode::session_closed;
}

ErrorCode SubscribedTrack::close() {
  if (auto session = session_.lock()) {
    session->close_track(endpoint_);
    return ErrorCode::ok;
  }
  return ErrorCode::session_closed;
}

ErrorCode SubscribedTrack::close_locked() {
  return close();
}

void SubscribedTrack::request_keyframe_async() {
  endpoint_->require_keyframe();
  if (auto session = session_.lock()) {
    session->request_keyframe(endpoint_);
  }
}

}  // namespace livekit::sdk::rtc
