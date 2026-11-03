#pragma once
#include "livekit/compat/span.hpp"
#include "livekit/compat/thread.hpp"
#include "livekit/result.hpp"
#include "livekit/transport_options.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace livekit::sdk::client {
enum class PublicationKind { audio = 0, video = 1, data = 2 };
enum class PublicationSource {
  unknown = 0,
  camera = 1,
  microphone = 2,
  screen = 3,
  screen_audio = 4
};

struct PublicationInfo {
  std::string sid, name, mime_type, stream;
  PublicationKind kind = PublicationKind::audio;
  PublicationSource source = PublicationSource::unknown;
  bool muted = false;
  uint32_t width = 0, height = 0;
};

struct ParticipantPermissions {
  bool can_subscribe = false, can_publish = false, can_publish_data = false;
  bool can_update_metadata = false, hidden = false;
  std::vector<PublicationSource> can_publish_sources;
};

struct ParticipantSnapshot {
  std::string sid, identity, name, metadata;
  std::map<std::string, std::string> attributes;
  std::map<std::string, PublicationInfo> publications;  // Server SID, not local TrackId.
  std::optional<ParticipantPermissions> permissions;    // Absent is unknown, not a denial.
  uint32_t version = 0;
  int state = 0, kind = 0, disconnect_reason = 0;
  bool speaking = false;
  float audio_level = 0;
  std::optional<int> connection_quality;
};

struct RoomInfo {
  std::string sid, name, metadata;
  bool recording = false;
};

struct PeerStatistics {
  int target = 0;  // 0: publisher, 1: subscriber.
  int64_t timestamp_us = 0;
  std::string json;  // WebRTC RTCStatsReport JSON, without native API types.
  std::map<std::string, std::string> publication_tracks;
  std::map<uint32_t, std::string> received_tracks;  // SSRC to remote Track SID.
};

struct ParticipantUpdate {
  // Wire semantics: empty name/metadata means unchanged. Attribute values of
  // "" delete those keys; omitted keys remain unchanged.
  std::string name, metadata;
  std::map<std::string, std::string> attributes;
};

namespace detail {
struct QueueBudget;
}

using TrackId = std::string;
using Epoch = uint64_t;
using SteadyClock = std::chrono::steady_clock;

enum class MediaKind { audio, video };
enum class Codec { opus, vp8, h264, h265 };
enum class TrackState { pending, ready, suspended, closed, failed };

// Immutable, owned encoded payload. Copies share storage; construction takes
// ownership. No native transport/codec/graph types cross this boundary.
class EncodedBuffer {
 public:
  EncodedBuffer() = default;

  explicit EncodedBuffer(std::vector<uint8_t> bytes)
      : bytes_(std::make_shared<const std::vector<uint8_t>>(std::move(bytes))) {}

  livekit::compat::span<const uint8_t> bytes() const {
    return bytes_ ? livekit::compat::span<const uint8_t>(*bytes_)
                  : livekit::compat::span<const uint8_t>{};
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

  bool operator==(const ReceiveDemand& other) const {
    return std::tie(disabled, quality, width, height, fps, priority) ==
           std::tie(other.disabled, other.quality, other.width, other.height, other.fps,
                    other.priority);
  }

  bool operator!=(const ReceiveDemand& other) const {
    return !(*this == other);
  }
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
  // Optional native receive metadata. Audio sequence numbers preserve loss and
  // reordering information across the encoded boundary; capture time is a sender
  // clock value, never interpreted as a local steady_clock timestamp.
  std::optional<uint16_t> sequence_number;
  std::optional<int64_t> sender_capture_time_us;
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
  Failure failure;
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
  std::shared_ptr<const FailureInfo> failure;
  bool muted = false;            // Current local publication gate, preserved on rejoin.
  ReceiveDemand receive_demand;  // Last locally submitted demand, not a server acknowledgement.
};

using TrackSid = std::string;
using ReceiveOptions = ReceiveDemand;
enum class ConnectionState {
  created,
  connecting,
  connected,
  reconnecting,
  failed,
  closing,
  closed
};

struct ReconnectOptions {
  bool enabled = true;
  size_t max_attempts = 6;
  std::chrono::milliseconds initial_delay{200}, max_delay{5000}, timeout{30000};
};

struct ConnectionOptions {
  MediaTransport media_transport = MediaTransport::automatic;
  std::string url, token;
  int connect_timeout = 30;
  bool auto_subscribe = true;
  size_t max_queued_bytes = 64 * 1024 * 1024, max_tracks = 128, max_events = 4096;
  std::chrono::milliseconds operation_timeout{30000};
  ReconnectOptions reconnect;
};

struct TrackPublishOptions {
  PublicationSource source = PublicationSource::unknown;
  std::string stream_group;
  std::vector<EncodingDescriptor> encodings{{"", 2000000, true}};
  bool follow_subscriber_demand = false;
};

struct WaitOptions {
  std::chrono::milliseconds timeout{1000};
  livekit::compat::stop_token stop;
};

struct DataPacket {
  std::vector<uint8_t> payload;
  bool reliable = true;
  std::string topic;
  std::vector<std::string> destinations;
};

struct PublicationSnapshot {
  TrackId track_id;
  TrackSid sid;
  std::string name;
  std::string participant_identity;
  MediaKind kind = MediaKind::video;
  PublicationSource source = PublicationSource::unknown;
  TrackState state = TrackState::pending;
  bool local = false;
  bool subscribed = false;
  bool desired = false;
  bool muted = false;
  Failure failure;
};
}  // namespace livekit::sdk::client
