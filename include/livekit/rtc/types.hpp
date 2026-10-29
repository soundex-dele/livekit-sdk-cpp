#pragma once
#include "livekit/rtc/signaling_options.hpp"
#include "livekit/rtc/session_info.hpp"
#include "livekit/encryption.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace livekit::sdk::rtc {

namespace detail {
struct QueueBudget;
}

using TrackId = std::string;
using Epoch = uint64_t;
using SteadyClock = std::chrono::steady_clock;

enum class MediaKind { audio, video };
enum class Codec { opus, vp8, h264, h265 };
enum class TrackState { pending, ready, suspended, closed, failed };
enum class ConnectionState { disconnected, connecting, connected, reconnecting, closed, failed };

// Immutable, owned encoded payload. Copies share storage; construction takes
// ownership. No native transport/codec/graph types cross this boundary.
class EncodedBuffer {
 public:
  EncodedBuffer() = default;

  explicit EncodedBuffer(std::vector<uint8_t> bytes)
      : bytes_(std::make_shared<const std::vector<uint8_t>>(std::move(bytes))) {}

  std::span<const uint8_t> bytes() const {
    return bytes_ ? std::span<const uint8_t>(*bytes_) : std::span<const uint8_t>{};
  }

  size_t size() const {
    return bytes_ ? bytes_->size() : 0;
  }

 private:
  friend struct detail::QueueBudget;
  std::shared_ptr<const std::vector<uint8_t>> bytes_;
  std::shared_ptr<void> reservation_;
  const detail::QueueBudget* budget_ = nullptr;
  SteadyClock::time_point admitted_{};
};

struct CodecFormat {
  Codec codec = Codec::h264;
  uint32_t clock_rate = 90000;
  uint16_t channels = 0;
  uint32_t width = 0, height = 0;
  // H26x access units use Annex-B; fmtp describes the declared negotiated codec.
  std::map<std::string, std::string> fmtp;
};

struct QueueLimits {
  size_t max_frames = 32;
  size_t max_bytes = 8 * 1024 * 1024;
  std::chrono::milliseconds max_age{250};
};

struct EncodingDescriptor {
  std::string id;
  uint32_t max_bitrate_bps = 2000000;
  bool active = true;
};

enum class ReceiveQuality { low = 0, medium = 1, high = 2 };

struct ReceiveDemand {
  bool disabled = false;
  ReceiveQuality quality = ReceiveQuality::high;  // Used when dimensions are zero.
  uint32_t width = 0, height = 0, fps = 0;  // Dimensions are either both zero or both positive.
  uint32_t priority = 0;                    // Zero: server default; 1 is highest priority.
  bool operator==(const ReceiveDemand&) const = default;
};

struct TrackDescriptor {
  TrackId id;  // Stable within this RoomEngine, never a server SID.
  std::string name;
  std::string source_id;
  std::string stream_group;
  // Subscription selector supplied by remote-track discovery. The local id
  // remains independent of the server publication binding.
  std::string remote_sid;
  MediaKind kind = MediaKind::video;
  CodecFormat format;
  QueueLimits queue;
  std::vector<EncodingDescriptor> encodings{{"", 2000000, true}};
  uint32_t priority = 1;                  // Relative scheduling weight, bounded by validation.
  bool muted = false;                     // Initial publication intent; not a subscription filter.
  bool follow_subscriber_demand = false;  // Single-layer video: honor server aggregate ON/OFF.
  // Signaled publication role, independent of producer identity (source_id).
  // Unknown remains unknown on the wire; camera/microphone are not inferred.
  PublicationSource source = PublicationSource::unknown;
  ReceiveDemand receive_demand;  // Initial subscription request; ignored for publications.
};

struct EncodedFrame {
  EncodedBuffer buffer;
  std::string encoding_id;
  uint64_t timestamp = 0;  // Unwrapped ticks in TrackDescriptor::format.clock_rate.
  std::chrono::microseconds duration{0};
  std::optional<SteadyClock::time_point> captured;
  Epoch epoch = 0;  // Required: use the endpoint's current binding epoch.
  uint64_t config_revision = 1;
  bool keyframe = false;
};

enum class WriteStatus {
  accepted,
  backpressure,
  keyframe_required,
  stale_epoch,
  not_ready,
  closed,
  invalid_frame
};
enum class ReadStatus { frame, timeout, cancelled, suspended, closed, failed };

struct ReadResult {
  ReadStatus status = ReadStatus::timeout;
  std::optional<EncodedFrame> frame;
};

struct EncodingFeedback {
  uint32_t target_bitrate_bps = 0;
  bool active = true;
  bool keyframe_required = false;
  uint64_t keyframe_requests = 0;
};

struct SenderFeedback {
  Epoch epoch = 0;
  uint64_t revision = 0;
  std::map<std::string, EncodingFeedback> encodings;
  uint64_t media_generation = 0;  // Changes at local mute boundaries, even between polls.
};

struct TrackSnapshot {
  TrackId id;
  std::string server_sid;
  TrackState state = TrackState::pending;
  Epoch epoch = 0;
  size_t queued_frames = 0, queued_bytes = 0;
  // All SDK-held access units for this endpoint, including native queues.
  size_t outstanding_frames = 0, outstanding_bytes = 0;
  uint64_t accepted = 0, consumed = 0, dropped = 0;
  std::string error;
  bool muted = false;            // Current local publication gate, preserved on rejoin.
  ReceiveDemand receive_demand;  // Last locally submitted demand, not a server acknowledgement.
};

struct ParticipantTrackPermission {
  std::string participant_identity;
  bool all_tracks = false;
  std::vector<TrackId> track_ids;  // Stable local publication IDs, including future publications.
};

struct SubscriptionPermissions {
  bool all_participants = true;
  std::vector<ParticipantTrackPermission> participants;
};

struct SubscriptionPermissionChange {
  std::string participant_sid, track_sid;
  bool allowed = false;
};

struct RoomOptions : SignalingOptions {
  EncryptionOptions encryption;
  std::optional<SubscriptionPermissions> subscription_permissions;
  size_t max_queued_bytes = 64 * 1024 * 1024;
  size_t max_tracks = 128;
  size_t max_events = 4096;
  std::chrono::milliseconds operation_timeout{30000};
  std::chrono::milliseconds stats_interval{0};  // Disabled, or 200..60000 ms.
};

struct TransportStatistics {
  int target = 0;  // 0 publisher, 1 subscriber.
  int64_t timestamp_us = 0;
  // JSON array of native W3C media/transport records. No candidate addresses,
  // certificates or credentials; absent native fields are not synthesized.
  std::string json;
  std::map<std::string, std::string>
      publication_tracks;  // Native track identifier -> current server SID.
};

struct RoomSnapshot {
  ConnectionState state = ConnectionState::disconnected;
  Epoch epoch = 0;
  size_t queued_bytes = 0;
  size_t publications = 0, subscriptions = 0;
  std::string error;
  // Null before the join response, after interruption and after termination.
  // Native join observations are released after transport establishment. Retained
  // snapshots remain historical values; compare epoch before using server SIDs.
  std::shared_ptr<const SessionInfo> session;
};

enum class RoomEventKind {
  connected,
  reconnecting,
  closed,
  failed,
  remote_track_added,
  remote_track_removed,
  track_ready,
  track_removed,
  track_failed,
  encryption_state_changed,
  session_changed,
  track_mute_changed,
  subscription_permission_changed,
  statistics
};

struct RoomEvent {
  RoomEventKind kind;
  TrackId track_id;
  std::optional<TrackDescriptor> descriptor;
  Epoch epoch = 0;
  std::string message;
  std::optional<EncryptionStateEvent> encryption;
  std::optional<SubscriptionPermissionChange> permission;
  std::shared_ptr<const TransportStatistics> statistics;
};

}  // namespace livekit::sdk::rtc
