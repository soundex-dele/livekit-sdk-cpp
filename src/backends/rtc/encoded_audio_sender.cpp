#include "backends/rtc/encoded_audio_sender.hpp"
#include "audio/channel_send_frame_transformer_delegate.h"
#include <stdexcept>
#include "backends/rtc/queue_budget.hpp"
#include <atomic>

namespace livekit::sdk::rtc::detail {

struct EncodedAudioSender::Budget {
  explicit Budget(QueueLimits limits) : limits(limits) {}
  std::mutex mutex;
  QueueLimits limits;
  size_t frames = 0, bytes = 0;
  std::atomic<bool> closed{false};
  std::atomic<bool> muted{false};
  std::atomic<uint64_t> generation{0};
};
struct EncodedAudioSender::Reservation {
  std::shared_ptr<Budget> budget;
  size_t bytes = 0;
  bool acquired = false;
  ~Reservation() {
    if (!acquired) return;
    std::lock_guard lock(budget->mutex);
    --budget->frames;
    budget->bytes -= bytes;
  }
};

EncodedAudioSender::EncodedAudioSender(BindingToken binding, QueueLimits limits, std::string encoding_id)
    : binding_(std::move(binding)), encoding_id_(std::move(encoding_id)), budget_(std::make_shared<Budget>(limits)) {
  if (!limits.max_frames || !limits.max_bytes || limits.max_age.count() <= 0)
    throw std::invalid_argument("Encoded audio sender needs bounded nonempty queues");
}

void EncodedAudioSender::configure(uint32_t ssrc, uint8_t payload_type) {
  if (!ssrc || payload_type > 127)
    throw std::invalid_argument("Invalid negotiated audio sender parameters");
  std::lock_guard lock(mutex_);
  if (closed_) return;
  ssrc_ = ssrc;
  payload_type_ = payload_type;
  configured_ = true;
}

SendResult EncodedAudioSender::send(const EncodedFrame& frame) {
  std::lock_guard lock(mutex_);
  if (closed_ || budget_->muted || frame.epoch != binding_.epoch || frame.buffer.size() == 0 ||
      frame.config_revision != 1 || frame.encoding_id != encoding_id_ ||
      frame.buffer.size() > budget_->limits.max_bytes)
    return SendResult::dropped;
  if (!configured_ || !callback_) return SendResult::would_block;
  auto reservation = std::make_shared<Reservation>();
  reservation->budget = budget_;
  reservation->bytes = frame.buffer.size();
  {
    std::lock_guard budget_lock(budget_->mutex);
    if (budget_->frames >= budget_->limits.max_frames ||
        reservation->bytes > budget_->limits.max_bytes - budget_->bytes)
      return SendResult::would_block;
    ++budget_->frames;
    budget_->bytes += reservation->bytes;
    reservation->acquired = true;
  }
  const auto bytes = frame.buffer.bytes();
  const auto deadline = QueueBudget::admitted(frame.buffer) + budget_->limits.max_age;
  auto native = webrtc::CreateSenderAudioFrame(
      {bytes.data(), bytes.size()}, payload_type_, uint32_t(frame.timestamp),
      ssrc_, "audio/opus", std::nullopt, std::nullopt,
      [reservation, room_reservation = QueueBudget::ownership(frame.buffer)] {}, [budget = budget_, deadline, generation = budget_->generation.load()] {
        return !budget->closed.load() && !budget->muted.load() && budget->generation == generation && SteadyClock::now() <= deadline;
      });
  // The native delegate only enqueues here. Its frame owns the reservation
  // until SendRtpAudio completes or the closed channel discards the task.
  callback_->OnTransformedFrame(std::move(native));
  return SendResult::sent;
}

void EncodedAudioSender::RegisterTransformedFrameCallback(
    webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback) {
  std::lock_guard lock(mutex_);
  if (!closed_) callback_ = std::move(callback);
}
void EncodedAudioSender::UnregisterTransformedFrameCallback() {
  std::lock_guard lock(mutex_);
  callback_ = nullptr;
}
void EncodedAudioSender::close() {
  std::lock_guard lock(mutex_);
  closed_ = true;
  budget_->closed = true;
  callback_ = nullptr;
}
void EncodedAudioSender::set_muted(bool muted) {
  std::lock_guard lock(mutex_);
  if (closed_ || budget_->muted == muted) return;
  budget_->muted = muted; ++budget_->generation;
}
size_t EncodedAudioSender::outstanding_frames() const {
  std::lock_guard lock(budget_->mutex);
  return budget_->frames;
}
size_t EncodedAudioSender::outstanding_bytes() const {
  std::lock_guard lock(budget_->mutex);
  return budget_->bytes;
}

} // namespace livekit::sdk::rtc::detail
