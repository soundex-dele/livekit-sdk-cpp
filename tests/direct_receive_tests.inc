// Deferred codec exercises completion after Decode() returns, without a GPU or
// desktop. The callback receives only RTP time, as real native decoders do.
class DeferredVideoDecoder final : public VideoDecoder {
 public:
  DecodedImageCallback* callback = nullptr;
  int result = 0, calls = 0, releases = 0;
  bool Configure(const Settings&) override { return true; }
  int32_t RegisterDecodeCompleteCallback(DecodedImageCallback* value) override { callback = value; return 0; }
  int32_t Decode(const EncodedImage&, int64_t) override { ++calls; return result; }
  int32_t Release() override { ++releases; return 0; }
  void complete(uint32_t timestamp) { auto frame = video_frame(timestamp); callback->Decoded(frame); }
};
static EncodedImage receive_packet(uint32_t ssrc, uint32_t timestamp) {
  EncodedImage packet;
  packet.SetRtpTimestamp(timestamp);
  packet.SetPacketInfos(RtpPacketInfos({RtpPacketInfo(ssrc, {}, timestamp, Clock::GetRealTimeClock()->CurrentTime())}));
  return packet;
}
static void direct_receive_lifecycle() {
  auto graph = std::make_shared<RtcGraph>();
  std::vector<livekit::sdk::VideoFrame> displayed;
  lk::VideoFrames output([&](const auto& frame) { displayed.push_back(frame); });
  auto a = std::make_shared<VideoReceiveGraph>(graph, "track-a", std::filesystem::path{}, nullptr, &output);
  auto b = std::make_shared<VideoReceiveGraph>(graph, "track-b", std::filesystem::path{}, nullptr, &output);
  check(a->start() && b->start(), "start independent receive graphs");
  graph->bind_video({101}, a); graph->bind_video({202}, b);
  bool collision = false;
  try { graph->bind_video({101, 303}, b); } catch (const std::runtime_error&) { collision = true; }
  check(collision && !graph->video_route(303), "conflicting SSRC binding is transactional");
  auto native_b = std::make_unique<DeferredVideoDecoder>(); auto* codec_b = native_b.get();
  auto native_a = std::make_unique<DeferredVideoDecoder>(); auto* codec_a = native_a.get();
  // Deliberately reverse factory order; matching is exclusively by RTP SSRC.
  auto decoder_b = create_graph_video_decoder(graph, "VP8", {}, std::move(native_b));
  auto decoder_a = create_graph_video_decoder(graph, "VP8", {}, std::move(native_a));
  VideoDecoder::Settings settings; settings.set_codec_type(kVideoCodecVP8);
  check(decoder_a->Configure(settings) && decoder_b->Configure(settings), "configure deferred codecs");
  DecodedFrames forbidden;
  decoder_a->RegisterDecodeCompleteCallback(&forbidden); decoder_b->RegisterDecodeCompleteCallback(&forbidden);
  auto packet_a = receive_packet(101, 1000), packet_b = receive_packet(202, 1000);
  packet_a.rotation_ = kVideoRotation_90;
  const auto present_ms = Clock::GetRealTimeClock()->TimeInMilliseconds() + 200;
  check(decoder_a->Decode(packet_a, present_ms) == 0 && decoder_b->Decode(packet_b, present_ms) == 0, "deferred requests accepted");
  codec_b->complete(1000); codec_a->complete(1000);
  output.dispatch(); check(displayed.empty(), "decoded frames wait until target presentation time");
  eventually([&] { output.dispatch(); return displayed.size() == 2; }, "both tracks independently presented");
  auto found = std::find_if(displayed.begin(), displayed.end(), [](const auto& f) { return f.track_id == "track-a"; });
  check(found != displayed.end() && found->width == 120 && found->height == 160 && found->timestamp_us == present_ms * 1000,
        "adapter restores target time and rotation without WebRTC output callbacks");
  check(forbidden.count == 0 && a->frames() == 1 && b->frames() == 1, "no callback into WebRTC or cross-track leakage");
  auto unknown = receive_packet(404, 2000);
  check(decoder_a->Decode(unknown, 0) < 0 && codec_a->calls == 1, "unknown SSRC never guessed");
  auto no_metadata = EncodedImage{};
  check(decoder_a->Decode(no_metadata, 0) < 0, "missing packet metadata rejected");
  auto stale = receive_packet(101, 3000);
  check(decoder_a->Decode(stale, 0) == 0, "async request before restart");
  a->stop(); check(a->start(), "restart receive graph");
  codec_a->complete(3000);
  check(a->frames() == 1, "old generation callback rejected after restart");
  auto failed = receive_packet(101, 4000);
  codec_a->result = -7;
  check(decoder_a->Decode(failed, 0) == -7, "native codec failure returned unchanged");
  codec_a->result = WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  check(decoder_a->Decode(failed, 0) == WEBRTC_VIDEO_CODEC_NO_OUTPUT, "no-output request cleans pending metadata");
  codec_a->result = 0;
  check(decoder_a->Decode(failed, 0) == 0, "timestamp reusable after failed/no-output submission");
  codec_a->complete(4000);
  eventually([&] { output.dispatch(); return displayed.size() == 3; }, "new generation renders");
  // A stalled native decoder cannot grow adapter metadata without bound.
  for (uint32_t i = 0; i < 64; ++i) check(decoder_a->Decode(receive_packet(101, 5000 + i), 0) == 0, "bounded outstanding request");
  check(decoder_a->Decode(receive_packet(101, 6000), 0) < 0, "overload reports error for keyframe recovery");
  codec_a->complete(5000);
  check(a->frames() == 2, "evicted metadata cannot deliver a stale picture");
  check(decoder_a->Decode(receive_packet(101, 7000), 0) == 0, "recovery after metadata overflow");
  graph->unbind_video(a.get()); a->stop(); codec_a->complete(7000);
  check(a->frames() == 2 && decoder_a->Decode(receive_packet(101, 8000), 0) < 0, "detached track refuses new and late frames");
  decoder_a->Release(); decoder_a->Release(); check(codec_a->releases == 1, "codec release is idempotent");
  graph->unbind_video(b.get()); b->stop();
  decoder_a.reset(); decoder_b.reset(); a.reset(); b.reset();
  check(graph->snapshot().empty(), "direct receive graphs release all nodes");
}

static void receive_presentation_bounds() {
  auto graph = std::make_shared<RtcGraph>();
  int displayed = 0;
  lk::VideoFrames output([&](const auto&) { ++displayed; });
  auto receiver = std::make_shared<VideoReceiveGraph>(graph, "bounded", std::filesystem::path{}, nullptr, &output);
  check(receiver->start(), "start bounded presenter");
  const auto time = Clock::GetRealTimeClock()->TimeInMicroseconds() + 1000000;
  for (uint32_t i = 0; i < 20; ++i) {
    auto frame = video_frame(i); frame.set_timestamp_us(time + i * 1000);
    receiver->decoded(receiver->generation(), frame);
  }
  auto nodes = graph->snapshot();
  auto presenter = std::find_if(nodes.begin(), nodes.end(), [](const auto& n) { return n.kind == "VideoPresentNode"; });
  check(presenter != nodes.end() && presenter->dropped == 12 && presenter->outputs == 0,
        "future presentation queue is bounded to eight decoded frames");
  receiver->stop(); output.dispatch();
  check(displayed == 0, "stop cancels future presentations and clears output targets");
}
class BlockingVideoDecoder final : public VideoDecoder {
 public:
  std::promise<void> entered, unblock;
  std::shared_future<void> released = unblock.get_future().share();
  bool Configure(const Settings&) override { return true; }
  int32_t RegisterDecodeCompleteCallback(DecodedImageCallback*) override { return 0; }
  int32_t Decode(const EncodedImage&, int64_t) override { entered.set_value(); released.wait(); return 0; }
  int32_t Release() override { return 0; }
};
static void receive_stop_barrier() {
  auto graph = std::make_shared<RtcGraph>();
  auto receiver = std::make_shared<VideoReceiveGraph>(graph, "barrier", std::filesystem::path{}, nullptr, nullptr);
  check(receiver->start(), "start decode barrier graph"); graph->bind_video({909}, receiver);
  auto native = std::make_unique<BlockingVideoDecoder>(); auto* codec = native.get();
  auto entered = codec->entered.get_future();
  auto decoder = create_graph_video_decoder(graph, "VP8", {}, std::move(native));
  VideoDecoder::Settings settings; check(decoder->Configure(settings), "configure blocking codec");
  DecodedFrames forbidden; decoder->RegisterDecodeCompleteCallback(&forbidden);
  auto decoding = std::async(std::launch::async, [&] { return decoder->Decode(receive_packet(909, 9000), 0); });
  const bool started = entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  if (!started) codec->unblock.set_value();
  check(started, "decode enters backend before stop");
  std::promise<void> stopping; auto stopping_started = stopping.get_future();
  auto stopped = std::async(std::launch::async, [&] { stopping.set_value(); receiver->stop(); });
  stopping_started.wait();
  const bool waited = stopped.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout;
  codec->unblock.set_value();
  const auto result = decoding.get(); stopped.get();
  check(waited && result == 0, "graph stop waits for in-flight synchronous decode without releasing codec on owner thread");
  graph->unbind_video(receiver.get()); decoder->Release();
}
