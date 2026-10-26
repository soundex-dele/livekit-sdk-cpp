#pragma once
#include "engines/rtc/rtc_graph.hpp"
#include "api/video/video_frame.h"
#include "media_graph/video_decode_target.hpp"
#include <filesystem>

namespace lk { class VideoPlayout; class VideoFrames; }
namespace livekit::sdk::detail {
// One executable graph per received track, from encoded ingress through display
// and recording. Codec adapters lease this graph via SSRC; no decoded frame is
// returned to WebRTC. start/stop are serialized by the track owner.
class VideoReceiveGraph final : public VideoDecodeTarget {
 public:
  VideoReceiveGraph(std::shared_ptr<RtcGraph> registry, std::string track_id,
                    const std::filesystem::path& recording, lk::VideoPlayout* playout,
                    lk::VideoFrames* frames);
  ~VideoReceiveGraph() override;
  bool start();
  void stop();
  uint64_t generation() const override;
  // The borrowed codec/frame run synchronously on WebRTC's decoder sequence;
  // its real codec return code is returned unchanged to WebRTC.
  int decode(uint64_t generation, webrtc::VideoDecoder& codec,
             const webrtc::EncodedImage& image, std::optional<bool> missing,
             int64_t render_time_ms, const std::string& format) override;
  void decoded(uint64_t generation, const webrtc::VideoFrame& frame) override;
  uint64_t frames() const;
  std::string error() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
