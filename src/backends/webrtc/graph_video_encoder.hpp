#pragma once
#include "engines/rtc/rtc_graph.hpp"
#include "media_graph/video_events.hpp"
#include "api/environment/environment.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/sdp_video_format.h"
#include <functional>
#include "media_nodes/capture/encoded_source.hpp"

namespace livekit::sdk::detail {
webrtc::VideoFrame external_encoded_video(EncodedMediaFrame frame,std::shared_ptr<EncodedSourceReader> source);
using VideoCodecCreator = std::function<std::unique_ptr<webrtc::VideoEncoder>(
    const webrtc::Environment&, const webrtc::SdpVideoFormat&)>;

// Creates an uninitialized node for the caller's GraphEngine. It subscribes
// to RawVideoFrame and publishes EncodedVideoFrame native packet envelopes.
// The caller connects sources/sinks and owns the complete graph lifecycle.
std::shared_ptr<MediaNode> create_video_encoder_node();

// Implements only the stock VideoEncoder API. Encode() unwraps the matching
// native envelope and forwards it; it never calls the real codec.
std::unique_ptr<webrtc::VideoEncoder> create_passthrough_video_encoder(
    const webrtc::Environment& env, const webrtc::SdpVideoFormat& format,
    std::shared_ptr<RtcGraph> graph, VideoCodecCreator create);
}
