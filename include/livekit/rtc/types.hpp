#pragma once
#include "livekit/media_core/video_decode_completion.hpp"
#include <tuple>
#include "livekit/compat/span.hpp"
#include "livekit/failure.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace livekit::sdk::rtc {

using TrackId = std::string;
using Epoch = uint64_t;
using SteadyClock = std::chrono::steady_clock;

enum class MediaKind { audio, video };
enum class Codec { opus, vp8, h264, h265 };
enum class TrackState { pending, ready, suspended, closed, failed };
enum class PublicationSource {
  unknown = 0,
  camera = 1,
  microphone = 2,
  screen = 3,
  screen_audio = 4
};

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
  std::shared_ptr<const std::vector<uint8_t>> bytes_;
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

struct TrackDescriptor {
  TrackId id;  // Local media graph identifier.
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
  // Optional native receive metadata. Audio sequence numbers preserve loss and
  // reordering information across the encoded boundary; capture time is a sender
  // clock value, never interpreted as a local steady_clock timestamp.
  std::optional<uint16_t> sequence_number;
  std::optional<int64_t> sender_capture_time_us;
  std::shared_ptr<media::VideoDecodeCompletion> decode_completion;
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

}  // namespace livekit::sdk::rtc
