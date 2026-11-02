#pragma once

#include "livekit/compat/thread.hpp"
#include "backends/rtc/room_transport.hpp"
#include "backends/rtc/queue_budget.hpp"
#include "livekit/result.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace livekit::sdk::rtc::detail {

Result<void> validate_descriptor(const TrackDescriptor&, size_t room_budget,
                                 bool publication = true);
Result<void> validate_receive_demand(const ReceiveDemand&, MediaKind);

struct Endpoint {
  Endpoint(TrackDescriptor, BindingToken, std::shared_ptr<QueueBudget>);
  ~Endpoint();
  const TrackDescriptor descriptor;
  const bool publication;

  struct Item {
    EncodedFrame frame;
    uint64_t serial;
    SteadyClock::time_point queued;
  };

  void attach_wakeup(std::shared_ptr<livekit::sdk::detail::WakeEvent>);
  std::optional<SteadyClock::time_point> next_expiration();
  TrackSnapshot snapshot() const;
  SenderFeedback feedback() const;
  std::optional<SenderFeedback> wait_feedback(Epoch, uint64_t, livekit::compat::stop_token);

  std::shared_ptr<QueueBudget> queue_budget() const {
    return budget_;
  }

  BindingToken binding() const;
  bool matches(const BindingToken&) const;
  void rebind(Epoch);
  void ready(std::string server_sid);
  void finish(TrackState, std::string error = {}, std::shared_ptr<const FailureInfo> = {});
  WriteStatus write(EncodedFrame);
  ReadResult read(std::chrono::milliseconds, livekit::compat::stop_token);
  ReadResult wait_frame(livekit::compat::stop_token);
  std::optional<Item> front();
  void consumed(uint64_t serial, bool dropped = false);
  void update_feedback(const SenderFeedback&);
  void set_muted(bool);
  void set_receive_demand(ReceiveDemand);
  void require_keyframe();

 private:
  ReadResult read_locked(livekit::compat::stop_token);
  void clear_locked();
  void require_keyframe_locked();
  void expire_locked();
  mutable std::mutex mutex_;
  livekit::compat::condition_variable_any changed_;
  std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup_;
  void notify_locked();
  BindingToken binding_;
  TrackSnapshot stats_;
  SenderFeedback feedback_;
  SenderFeedback transport_feedback_;
  std::map<std::string, std::optional<uint64_t>> last_timestamp_;
  std::deque<Item> queue_;
  std::shared_ptr<QueueBudget> budget_;
  uint64_t next_serial_ = 0;
  std::optional<SteadyClock::time_point> mute_boundary_;
};

}  // namespace livekit::sdk::rtc::detail
