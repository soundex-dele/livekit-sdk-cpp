#pragma once
#include "media_graph/rtc_graph.hpp"
#include "media_graph/video_events.hpp"
#include "api/environment/environment.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/sdp_video_format.h"
#include <functional>

namespace livekit::sdk::detail {
using VideoCodecCreator = std::function<std::unique_ptr<webrtc::VideoEncoder>(
    const webrtc::Environment&, const webrtc::SdpVideoFormat&)>;

// Raw frames enter this graph-owned node. Output frames are native envelopes
// containing encoded packets, never raw pixels to be encoded by WebRTC.
class VideoEncoderNode;
class GraphVideoSender {
 public:
  using Output = std::function<void(const webrtc::VideoFrame&)>;
  // Embedded mode contributes Created nodes to the track's GraphEngine.
  GraphVideoSender(std::shared_ptr<RtcGraph> graph, Output output, bool standalone = true);
  ~GraphVideoSender();
  void submit(const webrtc::VideoFrame& frame);
  void pause();
  void resume();
  std::vector<std::shared_ptr<MediaNode>> nodes() const;
  std::vector<RtcPipeline::Connection> connections() const;
  std::shared_ptr<MediaNode> input_node() const { return input_; }
 private:
  std::shared_ptr<VideoEncoderNode> impl_;
  std::shared_ptr<MediaEventNode<RawVideoFrame>> input_;
  std::shared_ptr<MediaEventNode<EncodedVideoFrame>> output_;
  std::unique_ptr<RtcPipeline> pipeline_;
};

// Implements only the stock VideoEncoder API. Encode() unwraps the matching
// native envelope and forwards it; it never calls the real codec.
std::unique_ptr<webrtc::VideoEncoder> create_passthrough_video_encoder(
    const webrtc::Environment& env, const webrtc::SdpVideoFormat& format,
    std::shared_ptr<RtcGraph> graph, VideoCodecCreator create);
}
