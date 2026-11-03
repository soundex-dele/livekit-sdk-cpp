#pragma once
#include "livekit/client/room_event.hpp"
#include "backends/livekit/track_config.hpp"
#include <atomic>
#include <functional>

namespace livekit::sdk::client::detail {
struct TrackData;

struct Operation {
  SteadyClock::time_point deadline;
  const std::atomic<bool>* stopped;

  Result<void> status() const {
    if (stopped->load()) {
      return Result<void>::failure(
          make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled, "Room is closing"));
    }
    if (SteadyClock::now() >= deadline) {
      return Result<void>::failure(
          make_failure(ErrorDomain::lifecycle, ErrorCode::timeout, "Operation deadline exceeded"));
    }
    return Result<void>::success();
  }
};

struct JoinState {
  RoomInfo room;
  ParticipantSnapshot local;
  std::vector<ParticipantSnapshot> participants;
};

struct SessionEvent {
  enum class Kind { participant, room, data, token, feedback, mute, quality, speakers } kind;
  ParticipantSnapshot participant;
  std::vector<ParticipantSnapshot> speakers;
  bool full_speaker_update = false;
  RoomInfo room;
  TrackSid sid;
  DataPacket data;
  std::string identity;
  std::string token;
  SenderFeedback feedback;
  bool muted = false;
  Failure failure;
};

// Engine-owner operations. This internal test seam owns no room business state.
// Rust EngineResult<T>/EngineResult<()> map to Result<T>/Result<void>.
// Results are true on success; Failure is only the error payload.
class RtcSession {
 public:
  using Events = std::function<void(SessionEvent)>;
  virtual ~RtcSession() = default;
  virtual Result<JoinState> connect(const ConnectionOptions&, Epoch, const Operation&) = 0;
  virtual Result<void> poll(const Operation&) = 0;
  virtual void close() = 0;
  virtual Result<PublicationInfo> add_track(const TrackConfig&, const Operation&) = 0;
  virtual Result<void> create_sender(const TrackSid&, std::shared_ptr<TrackData>,
                                     const Operation&) = 0;
  virtual Result<void> remove_track(const TrackSid&, const Operation&) = 0;
  virtual Result<void> subscribe(const TrackSid&, std::shared_ptr<TrackData>, bool,
                                 const Operation&) = 0;
  virtual Result<void> receive_options(const TrackSid&, ReceiveOptions) = 0;
  virtual Result<void> mute(const TrackSid&, bool) = 0;
  virtual Result<void> keyframe(const TrackSid&) = 0;
  virtual Result<void> publish_data(const DataPacket&, const Operation&) = 0;
  virtual Result<void> update(const ParticipantUpdate&, const Operation&) = 0;

  virtual Result<std::vector<PeerStatistics>> statistics(const Operation&) {
    return Result<std::vector<PeerStatistics>>::success({});
  }

  Events events;
};

using SessionFactory = std::function<std::unique_ptr<RtcSession>()>;
std::unique_ptr<RtcSession> create_rtc_session();
}  // namespace livekit::sdk::client::detail
