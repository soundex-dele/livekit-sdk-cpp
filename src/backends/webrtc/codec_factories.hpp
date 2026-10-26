#pragma once
#include "engines/rtc/rtc_graph.hpp"
#include "api/peer_connection_interface.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/audio_codecs/audio_decoder_factory.h"

namespace livekit::sdk::detail {
struct CodecBackends {
  std::shared_ptr<webrtc::VideoEncoderFactory> video_encoder;
  std::shared_ptr<webrtc::VideoDecoderFactory> video_decoder;
  webrtc::scoped_refptr<webrtc::AudioEncoderFactory> audio_encoder;
  webrtc::scoped_refptr<webrtc::AudioDecoderFactory> audio_decoder;
};
CodecBackends builtin_codecs();
// Call before EnableMediaWithDefaults: that function assembles the media engine.
void install_graph_codecs(webrtc::PeerConnectionFactoryDependencies& deps,
                          std::shared_ptr<RtcGraph> graph,
                          CodecBackends backends = builtin_codecs());
}
