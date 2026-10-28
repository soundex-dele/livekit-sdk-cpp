#include "quality.hpp"
#include <cmath>
#include <set>

namespace demo {
namespace {
using livekit::sdk::Json;
std::optional<double> number(const Json& row, const char* key) {
  auto it = row.find(key);
  if (it == row.end() || !it->is_number()) return {};
  const auto value = it->get<double>();
  return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}
std::string text(const Json& row, const char* key) {
  auto it = row.find(key);
  return it != row.end() && it->is_string() ? it->get<std::string>() : "";
}
std::optional<double> rate(std::optional<double> now, std::optional<double> before, double seconds) {
  if (!now || !before || *before < 0 || *now < *before || seconds <= 0 || seconds > 3) return {};
  return (*now - *before) / seconds;
}
}
bool PeerQuality::stale() const {
  return timestamp_us > 0 && std::chrono::steady_clock::now() - updated > std::chrono::seconds(3);
}
const char* quality_name(int quality) {
  switch (quality) {
    case 0: return "Poor";
    case 1: return "Good";
    case 2: return "Excellent";
    case 3: return "Lost";
    default: return "Unknown";
  }
}
void QualitySnapshot::update(const Json& event) {
  if (text(event, "event") == "media-graph-stats") { update_graph(event); return; }
  if (text(event, "event") == "graph-track-stats") {
    auto target = number(event, "target");
    auto tracks = event.find("tracks");
    if (!target || (*target != 0 && *target != 1) || tracks == event.end() || !tracks->is_array()) return;
    auto& mappings = remote_tracks[static_cast<size_t>(*target)];
    mappings.clear();
    for (const auto& track : *tracks) {
      auto ssrcs = track.find("ssrcs");
      const auto sid = text(track, "track");
      if (sid.empty() || ssrcs == track.end() || !ssrcs->is_array()) continue;
      for (const auto& ssrc : *ssrcs) if (ssrc.is_number_unsigned()) mappings[std::to_string(ssrc.get<uint64_t>())] = sid;
    }
    return;
  }
  auto target = number(event, "target"), timestamp = number(event, "timestampUs");
  auto rows = event.find("stats");
  if (!target || (*target != 0 && *target != 1) || !timestamp || *timestamp <= 0 ||
      rows == event.end() || !rows->is_array()) return;
  auto& previous = peers[static_cast<size_t>(*target)];
  // Ignore reordered/cached reports; they cannot form a rate interval.
  if (*timestamp <= previous.timestamp_us) return;
  PeerQuality next;
  next.timestamp_us = *timestamp;
  next.updated = std::chrono::steady_clock::now();
  std::set<std::string> selected_pairs;
  std::map<std::string, std::string> media_sources;
  for (const auto& row : *rows) if (text(row, "type") == "media-source")
    media_sources[text(row, "id")] = text(row, "trackIdentifier");
  for (const auto& row : *rows) if (text(row, "type") == "transport") {
    const auto id = text(row, "selectedCandidatePairId");
    if (!id.empty()) selected_pairs.insert(id);
  }
  const double seconds = (*timestamp - previous.timestamp_us) / 1e6;
  for (const auto& row : *rows) {
    const auto type = text(row, "type"), id = text(row, "id");
    if (type == "candidate-pair" && selected_pairs.contains(id)) {
      if (auto rtt = number(row, "currentRoundTripTime"); rtt && *rtt >= 0)
        next.rtt_ms = *rtt * 1000;
    }
    if ((type != "outbound-rtp" && type != "inbound-rtp") || id.empty()) continue;
    StreamQuality stream;
    stream.id = id;
    stream.sending = type == "outbound-rtp";
    stream.kind = text(row, "kind");
    if (stream.kind.empty()) stream.kind = text(row, "mediaType");
    if (auto ssrc = number(row, "ssrc"); ssrc && *ssrc >= 0 && *ssrc <= 4294967295.0)
      stream.ssrc = std::to_string(static_cast<uint32_t>(*ssrc));
    stream.bytes = number(row, stream.sending ? "bytesSent" : "bytesReceived");
    stream.retransmit_bytes = number(row, stream.sending ? "retransmittedBytesSent" : "retransmittedBytesReceived");
    // Native outbound stats do not expose FEC bytes. Do not infer them from
    // packet counts or from the difference between total and media bytes.
    if (!stream.sending) stream.fec_bytes = number(row, "fecBytesReceived");
    stream.track_id = text(row, "trackIdentifier");
    if (stream.sending) {
      auto source = media_sources.find(text(row, "mediaSourceId"));
      if (source != media_sources.end()) stream.track_id = source->second;
    }
    if (stream.kind == "video" && stream.sending) stream.frames = number(row, "framesSent");
    if (!stream.sending) {
      stream.lost = number(row, "packetsLost");
      if (auto jitter = number(row, "jitter")) stream.jitter_ms = *jitter * 1000;
    }
    const auto old = previous.streams.find(id);
    // First samples, long gaps, SSRC changes and counter resets need a baseline.
    if (previous.timestamp_us > 0 && seconds > 0 && seconds <= 3 && old != previous.streams.end() &&
        old->second.ssrc == stream.ssrc && old->second.sending == stream.sending && old->second.kind == stream.kind &&
        old->second.track_id == stream.track_id) {
      if (auto bytes = rate(stream.bytes, old->second.bytes, seconds)) stream.kbps = *bytes * 8 / 1000;
      if (auto bytes = rate(stream.retransmit_bytes, old->second.retransmit_bytes, seconds)) stream.retransmit_kbps = *bytes * 8 / 1000;
      if (auto bytes = rate(stream.fec_bytes, old->second.fec_bytes, seconds)) stream.fec_kbps = *bytes * 8 / 1000;
      stream.fps = rate(stream.frames, old->second.frames, seconds);
    }
    next.streams.emplace(id, std::move(stream));
  }
  previous = std::move(next);
}
void QualitySnapshot::update_graph(const Json& event) {
  auto timestamp = number(event, "timestampUs");
  auto rows = event.find("nodes"), edges = event.find("edges");
  if (!timestamp || *timestamp <= 0 || *timestamp <= graph_sample.timestamp_us ||
      rows == event.end() || !rows->is_array() || edges == event.end() || !edges->is_array()) return;
  std::set<std::string> raw_sources;
  for (const auto& edge : *edges)
    if (text(edge, "dataType").find("RawVideoFrame") != std::string::npos) raw_sources.insert(text(edge, "from"));
  std::map<uint64_t, NodeQuality> next;
  const double seconds = (*timestamp - graph_sample.timestamp_us) / 1e6;
  for (const auto& row : *rows) {
    auto id = row.find("id");
    auto generation = number(row, "generation");
    if (id == row.end() || !id->is_number_unsigned() || !generation) continue;
    const auto kind = text(row, "kind"), name = text(row, "name");
    Stage stage;
    if (kind == "VideoEncoderNode") stage = Stage::encode;
    else if (kind == "VideoDecoderNode") stage = Stage::decode;
    else if (kind == "RtcEncodedSourceNode") stage = Stage::receive;
    else if ((kind == "VideoSourceNode" || kind == "CameraCaptureNode" || kind == "ScreenCaptureNode") &&
             raw_sources.contains(name)) stage = Stage::capture;
    else continue;
    const auto key = id->get<uint64_t>();
    NodeQuality node{name, text(row, "trackId"), stage, *generation, number(row, "outputs"), {}};
    auto old = nodes.find(key);
    if (graph_sample.timestamp_us > 0 && old != nodes.end() && old->second.generation == node.generation &&
        old->second.stage == stage && old->second.name == name && old->second.track_id == node.track_id)
      node.fps = rate(node.frames, old->second.frames, seconds);
    next.emplace(key, std::move(node));
  }
  nodes = std::move(next);
  local_tracks.clear();
  if (auto tracks = event.find("localTracks"); tracks != event.end() && tracks->is_object())
    for (auto it = tracks->begin(); it != tracks->end(); ++it)
      if (it.value().is_string()) local_tracks[it.key()] = it.value().get<std::string>();
  graph_sample.timestamp_us = *timestamp;
  graph_sample.updated = std::chrono::steady_clock::now();
}
std::string QualitySnapshot::track_id(const StreamQuality& stream, size_t peer) const {
  if (stream.sending) {
    auto mapped = local_tracks.find(stream.track_id);
    return mapped == local_tracks.end() ? stream.track_id : mapped->second;
  }
  auto mapped = remote_tracks[peer].find(stream.ssrc);
  return mapped == remote_tracks[peer].end() ? stream.track_id : mapped->second;
}
std::optional<double> QualitySnapshot::bitrate(Bitrate kind, const std::string& track, bool sending) const {
  if (track.empty()) return {};
  std::optional<double> total;
  for (size_t p = 0; p < peers.size(); ++p) for (const auto& [id, stream] : peers[p].streams) {
    if (stream.sending != sending || track_id(stream, p) != track) continue;
    if (peers[p].stale() || (sending && stream.track_id != track && graph_sample.stale())) return {};
    const auto value = kind == Bitrate::rtp ? stream.kbps :
      kind == Bitrate::retransmit ? stream.retransmit_kbps : stream.fec_kbps;
    // A new simulcast layer or missing counter invalidates only this track's total.
    if (!value) return {};
    total = total.value_or(0) + *value;
  }
  return total;
}
std::optional<double> QualitySnapshot::fps(Stage stage, const std::string& track) const {
  std::optional<double> total;
  if (stage == Stage::send) {
    if (!track.empty() && graph_sample.stale()) return {};
    for (size_t p = 0; p < peers.size(); ++p) for (const auto& [id, stream] : peers[p].streams) {
      const auto& peer = peers[p];
      if (stream.kind != "video" || !stream.sending || (!track.empty() && track_id(stream, p) != track)) continue;
      if (peer.stale() || !stream.fps) return {};
      total = total.value_or(0) + *stream.fps;
    }
  } else {
    if (graph_sample.stale()) return {};
    for (const auto& [id, node] : nodes) if (node.stage == stage && (track.empty() || node.track_id == track)) {
      if (!node.fps) return {};
      total = total.value_or(0) + *node.fps;
    }
  }
  return total;
}
}
