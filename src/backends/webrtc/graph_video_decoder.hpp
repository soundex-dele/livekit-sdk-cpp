#pragma once
#include "media_graph/rtc_graph.hpp"
#include "api/video_codecs/video_decoder.h"

namespace livekit::sdk::detail {
std::unique_ptr<webrtc::VideoDecoder> create_graph_video_decoder(
    std::shared_ptr<RtcGraph> graph, std::string format, std::shared_ptr<void> owner,
    std::unique_ptr<webrtc::VideoDecoder> backend);
}
