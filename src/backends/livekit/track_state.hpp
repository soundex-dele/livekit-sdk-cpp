#pragma once
#include "backends/livekit/rtc_engine.hpp"
#include "backends/livekit/media/queue_budget.hpp"

namespace livekit::sdk::client::detail {
struct TrackData {
  mutable std::mutex mutex;
  std::mutex reader;
  std::condition_variable changed;
  TrackConfig config;
  TrackSnapshot info;
  SenderFeedback feedback;
  std::weak_ptr<RtcEngine> engine;
  bool claimed = false, needs_keyframe = true;
  std::optional<uint64_t> last_timestamp;
  std::shared_ptr<QueueBudget> budget;
  std::deque<EncodedFrame> frames;
  std::function<WriteStatus(const EncodedFrame&)> send;
  std::function<SenderFeedback()> sender_feedback;
  std::function<void(bool)> mute_sender;
  void transition(TrackState, Epoch = 0, TrackSid = {});
  WriteStatus write(EncodedFrame);
  bool receive(EncodedFrame);
  ReadResult read(WaitOptions);
  TrackSnapshot snapshot() const;
};

Result<void> validate_track(const TrackConfig&);
}  // namespace livekit::sdk::client::detail
