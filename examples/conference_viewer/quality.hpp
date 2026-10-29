#pragma once
#include <livekit/json.hpp>
#include <array>
#include <chrono>
#include <map>
#include <optional>

namespace demo {
struct StreamQuality {
  std::string id, kind, ssrc, track_id;
  bool sending = false;
  // Send FPS uses framesSent. Receive FPS comes from graph ingress because
  // WebRTC's framesReceived depends on the bypassed decoded-frame callback.
  std::optional<double> bytes, kbps, frames, fps, lost, jitter_ms;
  std::optional<double> retransmit_bytes, retransmit_kbps, fec_bytes, fec_kbps;
};
struct PeerQuality {
  double timestamp_us = 0;
  std::chrono::steady_clock::time_point updated{};
  std::optional<double> rtt_ms;
  std::map<std::string, StreamQuality> streams;
  bool stale() const;
};
struct QualitySnapshot {
  // Publisher and subscriber PeerConnections have independent counters/clocks.
  std::array<PeerQuality, 2> peers;
  std::map<std::string, int> participants;
  enum class Stage { capture, encode, send, receive, decode };
  enum class Bitrate { rtp, retransmit, fec };
  struct NodeQuality {
    std::string name, track_id;
    Stage stage;
    double generation = 0;
    std::optional<double> frames, fps;
  };
  // Graph and RTP reports have independent sampling clocks.
  PeerQuality graph_sample;
  std::map<uint64_t, NodeQuality> nodes;
  std::map<std::string, std::string> local_tracks;
  std::array<std::map<std::string, std::string>, 2> remote_tracks;
  std::optional<double> fps(Stage stage, const std::string& track_id = {}) const;
  std::optional<double> bitrate(Bitrate kind, const std::string& track_id, bool sending) const;
  std::string track_id(const StreamQuality& stream, size_t peer) const;
  void update(const livekit::sdk::Json& event);
  void update_graph(const livekit::sdk::Json& event);
};
const char* quality_name(int quality);
}
