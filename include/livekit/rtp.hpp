#pragma once
#include "livekit/encoded_media.hpp"
#include <span>

namespace livekit::sdk {
namespace detail {class PacketEventLog;}
struct PacketEvent {
  bool incoming=false,rtcp=false;
  // RTP contains the header/extensions only; RTCP contains the compound packet.
  // Native SRTP, pacing, retransmission and sequence ownership remain internal.
  std::vector<uint8_t> bytes;
  size_t packet_size=0;
  uint64_t generation=0;
  std::chrono::steady_clock::time_point observed;
};
class PacketStream {
 public:
  explicit PacketStream(size_t capacity=512);
  ~PacketStream();
  std::optional<PacketEvent> read(std::chrono::milliseconds timeout=std::chrono::seconds(30),std::stop_token stop={});
  void close();
  uint64_t dropped()const;
 private:
  struct Impl;std::unique_ptr<Impl> impl_;
  void push(PacketEvent event);
  friend class detail::PacketEventLog;
};
// Maps one SSRC's 32-bit RTP clock using RTCP sender reports. Reset on source
// changes; mappings farther than half a timestamp cycle are ambiguous.
class RtpClock {
 public:
  explicit RtpClock(uint32_t rate);
  void sender_report(uint32_t rtp,uint64_t ntp);
  std::optional<std::chrono::nanoseconds> ntp_time(uint32_t rtp)const;
  void reset();
 private:
  uint32_t rate_,rtp_=0;std::optional<std::chrono::nanoseconds> ntp_;
};
// Accepts ordered RTP for an externally encoded source. Depacketizes into
// access units; libwebrtc assigns negotiated PT/SSRC/extensions/sequence and
// performs pacing, SRTP and retransmission. This is not byte-exact RTP replay.
class RtpMediaSource {
 public:
  RtpMediaSource(EncodedSourceOptions options,uint8_t payload_type,uint32_t ssrc);
  ~RtpMediaSource();
  std::shared_ptr<EncodedMediaSource> source()const;
  EncodedWriteStatus write(std::span<const uint8_t> packet);
  void reset();
 private:
  struct Impl;std::unique_ptr<Impl> impl_;
};
}
