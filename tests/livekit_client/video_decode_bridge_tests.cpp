#include "backends/livekit/media/encoded_video_sender.hpp"
#include "backends/livekit/media/video_receive_bridge.hpp"
#include "backends/livekit/track_state.hpp"
#include "livekit/media.hpp"
#include "api/environment/environment_factory.h"
#include "api/rtp_packet_info.h"
#include "modules/video_coding/include/video_error_codes.h"
#include <cstdlib>
#include <iostream>

using namespace livekit::sdk::client;
using namespace livekit::sdk::client::detail;
using namespace std::chrono_literals;

static void check(bool ok, const char* message) {
  if (!ok) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

struct DecodeSink : webrtc::DecodedImageCallback {
  int count = 0;
  uint32_t timestamp = 0;

  int32_t Decoded(webrtc::VideoFrame& frame) override {
    ++count;
    timestamp = frame.rtp_timestamp();
    check(frame.width() == 4 && frame.height() == 4, "actual decoded dimensions");
    check(frame.video_frame_buffer()->ToI420()->DataY()[0] == 73, "actual decoded pixels");
    return WEBRTC_VIDEO_CODEC_OK;
  }
};

int main() {
  auto routes = std::make_shared<VideoReceiveRoutes>();
  auto route = std::make_shared<VideoReceiveRoute>();
  auto track = std::make_shared<TrackData>();
  track->config.kind = MediaKind::video;
  track->budget = std::make_shared<QueueBudget>(1024, 4);
  track->transition(TrackState::ready, 1);
  route->track = track;
  check(routes->add(123, route), "register route");
  check(!routes->add(123, std::make_shared<VideoReceiveRoute>()), "reject SSRC collision");
  auto factory = create_transport_video_decoder_factory(routes);
  auto decoder = factory->Create(webrtc::CreateEnvironment(), webrtc::SdpVideoFormat("VP8"));
  check(bool(decoder), "create boundary decoder");
  webrtc::VideoDecoder::Settings settings;
  check(decoder->Configure(settings), "configure boundary");
  DecodeSink callback;
  decoder->RegisterDecodeCompleteCallback(&callback);
  auto pixels = std::make_shared<livekit::sdk::VideoFrame>();
  pixels->width = pixels->height = pixels->stride_y = 4;
  pixels->stride_u = pixels->stride_v = 2;
  pixels->y.assign(16, 73);
  pixels->u.assign(4, 128);
  pixels->v.assign(4, 128);
  auto submit = [&](uint32_t timestamp) {
    webrtc::EncodedImage image;
    const uint8_t bytes[] = {1, 2, 3};
    image.SetEncodedData(webrtc::EncodedImageBuffer::Create(bytes, sizeof(bytes)));
    image.SetRtpTimestamp(timestamp);
    image._frameType = webrtc::VideoFrameType::kVideoFrameKey;
    webrtc::RtpPacketInfo packet;
    packet.set_ssrc(123);
    image.SetPacketInfos(webrtc::RtpPacketInfos({packet}));
    check(decoder->Decode(image, 0) == WEBRTC_VIDEO_CODEC_OK, "accepted external decode");
    auto result = track->read({0ms, {}});
    check(result.status == ReadStatus::frame && result.frame->decode_completion,
          "encoded frame reaches media consumer with completion");
    return result.frame->decode_completion;
  };
  auto first = submit(90000);
  check(callback.count == 0, "no fabricated decode callback");
  first->decoded(pixels);
  first->decoded(pixels);
  check(callback.count == 1 && callback.timestamp == 90000, "exactly one real completion");
  auto late = submit(93000);
  decoder->Release();
  decoder->Configure(settings);
  decoder->RegisterDecodeCompleteCallback(&callback);
  late->decoded(pixels);
  check(callback.count == 1, "release invalidates old completion after re-registration");
  auto rebound = submit(96000);
  ++route->generation;
  rebound->decoded(pixels);
  check(callback.count == 1, "rebind invalidates old completion");
  auto closed = submit(99000);
  track->transition(TrackState::closed);
  closed->decoded(pixels);
  check(callback.count == 1, "close invalidates old completion");
  decoder.reset();
  routes->remove(123, route);
  check(!routes->find(123), "route removed");
  std::cout << "PASS video decode bridge: real pixels, once, release, rebind, close\n";
}
