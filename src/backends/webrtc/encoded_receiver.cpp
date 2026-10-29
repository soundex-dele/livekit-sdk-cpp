#include "backends/webrtc/encoded_receiver.hpp"
#include <stdexcept>
#include "backends/rtc/queue_budget.hpp"
#include <utility>

namespace livekit::sdk::rtc::detail {
EncodedReceiver::EncodedReceiver(MediaKind kind, QueueLimits limits,
                                 std::shared_ptr<QueueBudget> budget, PayloadTransform transform)
    : transform_(std::move(transform)),
      kind_(kind),
      limits_(limits),
      budget_(std::move(budget)),
      needs_keyframe_(kind == MediaKind::video),
      request_keyframe_(kind == MediaKind::video) {
  if (!limits.max_frames || !limits.max_bytes || limits.max_age.count() <= 0)
    throw std::invalid_argument("Encoded receiver needs bounded nonempty queues");
}

void EncodedReceiver::discard_chain() {
  dropped_ += queue_.size();
  queue_.clear();
  bytes_ = 0;
  if (kind_ == MediaKind::video)
    needs_keyframe_ = request_keyframe_ = true;
}

void EncodedReceiver::configure_queue(QueueLimits limits, std::shared_ptr<QueueBudget> budget) {
  if (!budget || !limits.max_frames || !limits.max_bytes || limits.max_age.count() <= 0)
    throw std::invalid_argument("Invalid subscribed receive queue");
  std::lock_guard lock(mutex_);
  discard_chain();
  limits_ = limits;
  budget_ = std::move(budget);
}

void EncodedReceiver::attach_wakeup(std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup) {
  std::lock_guard lock(mutex_);
  wakeup_ = std::move(wakeup);
  if (wakeup_) {
    wakeup_->signal();
  }
}

void EncodedReceiver::Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame) {
  try {
    receive(std::move(frame));
  } catch (...) {
    // Allocation/metadata failures must not unwind through a native callback.
    std::lock_guard lock(mutex_);
    discard_chain();
    ++dropped_;
  }
  std::lock_guard lock(mutex_);
  if (wakeup_) {
    wakeup_->signal();
  }
}

void EncodedReceiver::receive(std::unique_ptr<webrtc::TransformableFrameInterface> frame) {
  if (!frame)
    return;
  std::lock_guard lock(mutex_);
  if (closed_)
    return;
  const auto now = SteadyClock::now();
  auto* video = dynamic_cast<webrtc::TransformableVideoFrameInterface*>(frame.get());
  auto* audio = dynamic_cast<webrtc::TransformableAudioFrameInterface*>(frame.get());
  const auto encrypted = frame->GetData();
  std::span<const uint8_t> data(encrypted.data(), encrypted.size());
  std::optional<std::vector<uint8_t>> plaintext;
  if ((kind_ == MediaKind::video && !video) || (kind_ == MediaKind::audio && !audio) ||
      data.empty()) {
    ++dropped_;
    return;
  }
  if (data.size() > (transform_ ? limits_.max_bytes * 2 + 64 : limits_.max_bytes)) {
    discard_chain();
    ++dropped_;
    return;
  }
  if (transform_) {
    plaintext = transform_(data);
    if (!plaintext) {
      discard_chain();
      return;
    }
    data = *plaintext;
    if (data.empty() || data.size() > limits_.max_bytes) {
      discard_chain();
      ++dropped_;
      return;
    }
  }
  if ((!queue_.empty() && now - queue_.front().arrival > limits_.max_age) ||
      queue_.size() >= limits_.max_frames || data.size() > limits_.max_bytes - bytes_)
    discard_chain();
  if (video && needs_keyframe_ && !video->IsKeyFrame()) {
    ++dropped_;
    return;
  }
  ReceivedAccessUnit unit;
  if (budget_) {
    auto buffer = budget_->copy({data.data(), data.size()});
    if (!buffer) {
      discard_chain();
      ++dropped_;
      return;
    }
    unit.buffer = std::move(*buffer);
  } else
    unit.buffer = EncodedBuffer(std::vector<uint8_t>(data.begin(), data.end()));
  unit.timestamp = frame->GetTimestamp();
  if (auto capture = frame->CaptureTime(); capture && capture->IsFinite()) {
    unit.sender_capture_time_us = capture->us();
    if (auto offset = frame->SenderCaptureTimeOffset(); offset && offset->IsFinite()) {
      *unit.sender_capture_time_us += offset->us();
    }
  }
  if (kind_ == MediaKind::audio) {
    unit.sequence_number =
        static_cast<webrtc::TransformableAudioFrameInterface*>(frame.get())->SequenceNumber();
  }
  unit.ssrc = frame->GetSsrc();
  unit.mime_type = frame->GetMimeType();
  if (video) {
    unit.keyframe = video->IsKeyFrame();
    const auto metadata = video->Metadata();
    unit.width = metadata.GetWidth();
    unit.height = metadata.GetHeight();
    if (unit.keyframe)
      needs_keyframe_ = request_keyframe_ = false;
  }
  queue_.push_back({std::move(unit), now});
  bytes_ += data.size();
}

std::vector<ReceivedAccessUnit> EncodedReceiver::take_frames() {
  std::lock_guard lock(mutex_);
  if (!queue_.empty() && SteadyClock::now() - queue_.front().arrival > limits_.max_age)
    discard_chain();
  std::vector<ReceivedAccessUnit> result;
  result.reserve(queue_.size());
  for (auto& entry : queue_)
    result.push_back(std::move(entry.frame));
  queue_.clear();
  bytes_ = 0;
  return result;
}

bool EncodedReceiver::take_keyframe_request() {
  std::lock_guard lock(mutex_);
  return std::exchange(request_keyframe_, false);
}

void EncodedReceiver::close() {
  std::lock_guard lock(mutex_);
  closed_ = true;
  discard_chain();
  request_keyframe_ = false;
}

uint64_t EncodedReceiver::dropped() const {
  std::lock_guard lock(mutex_);
  return dropped_;
}
}  // namespace livekit::sdk::rtc::detail
