#include "backends/webrtc/codec_factories.hpp"
#include "backends/webrtc/audio_device.hpp"
#include "media_graph/encode_executor.hpp"
#include "system_wrappers/include/clock.h"
#include "media_graph/realtime_channel.hpp"
#include "lk/headless_audio.hpp"
#include "lk/media.hpp"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/video_codec.h"
#include "api/make_ref_counted.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <condition_variable>
#include <future>

using namespace webrtc;
using namespace livekit::sdk::detail;
static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static bool has_edge(const std::shared_ptr<RtcGraph>& graph, const std::string& from,
                     const std::string& to, const std::string& type) {
  std::map<std::string, std::string> kinds;
  for (const auto& node : graph->snapshot()) kinds[node.name] = node.kind;
  for (const auto& edge : graph->connections())
    if (kinds[edge.from] == from && kinds[edge.to] == to && edge.data_type == type) return true;
  return false;
}
static webrtc::VideoFrame video_frame(uint32_t timestamp = 9000) {
  auto buffer = I420Buffer::Create(160, 120);
  std::memset(buffer->MutableDataY(), 100, buffer->StrideY() * 120);
  std::memset(buffer->MutableDataU(), 90, buffer->StrideU() * 60);
  std::memset(buffer->MutableDataV(), 160, buffer->StrideV() * 60);
  return webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_rtp_timestamp(timestamp).set_timestamp_us(100000).build();
}
struct Encoded : EncodedImageCallback {
  std::optional<EncodedImage> frame;
  CodecSpecificInfo info{};
  int count = 0;
  Result OnEncodedImage(const EncodedImage& image, const CodecSpecificInfo* codec) override {
    frame = image; if (codec) info = *codec; ++count; return Result(Result::OK);
  }
};
struct DecodedFrames : DecodedImageCallback {
  int count = 0; uint32_t timestamp = 0;
  int32_t Decoded(webrtc::VideoFrame& frame) override {
    check(frame.width() == 160 && frame.height() == 120, "decoded dimensions");
    check(frame.video_frame_buffer()->ToI420()->DataY()[0] > 80, "decoded actual pixels");
    timestamp = frame.rtp_timestamp(); ++count; return 0;
  }
};
static void audio_codecs() {
  auto env = CreateEnvironment(); auto graph = std::make_shared<RtcGraph>();
  PeerConnectionFactoryDependencies deps; install_graph_codecs(deps, graph);
  SdpAudioFormat format("opus", 48000, 2, {{"minptime", "10"}, {"useinbandfec", "1"}});
  AudioEncoderFactory::Options options; options.payload_type = 111;
  auto encoder = deps.audio_encoder_factory->Create(env, format, options);
  auto decoder = deps.audio_decoder_factory->Create(env, format, std::nullopt);
  check(encoder && decoder, "Opus graph codec creation");
  auto backend = builtin_codecs(); auto reference = backend.audio_encoder->Create(env, format, options);
  check(encoder->SetFec(true) == reference->SetFec(true), "FEC forwarded");
  check(encoder->SetDtx(false) == reference->SetDtx(false), "DTX forwarded");
  encoder->OnReceivedTargetAudioBitrate(32000); reference->OnReceivedTargetAudioBitrate(32000);
  check(encoder->SampleRateHz() == reference->SampleRateHz() && encoder->NumChannels() == reference->NumChannels(), "audio format");
  std::vector<int16_t> samples(480 * encoder->NumChannels());
  Buffer encoded, expected;
  AudioEncoder::EncodedInfo result;
  for (int block = 0; block < 4 && encoded.empty(); ++block) {
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<int16_t>(5000 * std::sin((i + block * samples.size()) * 0.04));
    result = encoder->Encode(block * 480, samples, &encoded);
    reference->Encode(block * 480, samples, &expected);
  }
  check(encoded.size() > 0 && encoded.size() == expected.size() && std::memcmp(encoded.data(), expected.data(), encoded.size()) == 0,
        "node preserves native Opus bitstream");
  auto frames = decoder->ParsePayload(std::move(encoded), result.encoded_timestamp);
  check(!frames.empty(), "NetEq parse path");
  // Parsed frame leases keep decoder resources alive even if adapter is released.
  decoder.reset(); deps.audio_decoder_factory = nullptr;
  std::vector<int16_t> output(5760 * 2);
  auto decoded = frames.back().frame->Decode(output);
  check(decoded && decoded->num_decoded_samples > 0, "actual parsed-frame decode");
  bool nonzero = false; for (size_t i = 0; i < decoded->num_decoded_samples; ++i) nonzero |= output[i] != 0;
  check(nonzero, "decoded PCM contains signal");
  auto nodes = graph->snapshot();
  check(nodes.size() == 2 && nodes[0].inputs >= 1 && nodes[0].outputs == 1 && nodes[1].inputs == 1 && nodes[1].outputs == 1,
        "audio encode/decode go through independent graph nodes");
  frames.clear(); encoder.reset(); check(graph->snapshot().empty(), "parsed frame releases backend lease");
}

template<class F> static void eventually(F condition, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!condition() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  check(condition(), message);
}
struct TriggerFrames {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<webrtc::VideoFrame> frames;
  std::thread::id producer;
  void push(const webrtc::VideoFrame& frame) {
    std::lock_guard lock(mutex); producer = std::this_thread::get_id();
    frames.push_back(frame); ready.notify_all();
  }
  webrtc::VideoFrame take() {
    std::unique_lock lock(mutex);
    check(ready.wait_for(lock, std::chrono::seconds(5), [&] { return !frames.empty(); }), "graph output timeout");
    auto frame = frames.front(); frames.pop_front(); return frame;
  }
};
static void graph_driven_video() {
  auto env = CreateEnvironment(); auto graph = std::make_shared<RtcGraph>();
  PeerConnectionFactoryDependencies deps; install_graph_codecs(deps, graph);
  auto encoder = deps.video_encoder_factory->Create(env, SdpVideoFormat("VP8"));
  auto encoder2 = deps.video_encoder_factory->Create(env, SdpVideoFormat("VP8"));
  auto decoder = deps.video_decoder_factory->Create(env, SdpVideoFormat("VP8"));
  check(encoder && encoder2 && decoder, "factory creates passthrough and decoder");
  check(encoder->GetEncoderInfo().supports_native_handle, "native encoded envelope supported");
  VideoCodec config; config.codecType = kVideoCodecVP8; config.width = 160; config.height = 120;
  config.maxFramerate = 30; config.startBitrate = 200; config.minBitrate = 30; config.maxBitrate = 500;
  config.active = true; config.qpMax = 56; config.mode = VideoCodecMode::kRealtimeVideo;
  *config.VP8() = VideoEncoder::GetDefaultVp8Settings();
  VideoEncoder::Settings settings(VideoEncoder::Capabilities(false), 1, 1200);
  Encoded encoded, encoded2;
  check(encoder->InitEncode(&config, settings) == 0 && encoder2->InitEncode(&config, settings) == 0, "passthrough init");
  encoder->RegisterEncodeCompleteCallback(&encoded); encoder2->RegisterEncodeCompleteCallback(&encoded2);
  VideoBitrateAllocation allocation; allocation.SetBitrate(0, 0, 180000);
  encoder->SetRates(VideoEncoder::RateControlParameters(allocation, 30));
  encoder2->SetRates(VideoEncoder::RateControlParameters(allocation, 30));
  TriggerFrames triggers, triggers2;
  auto sender = std::make_unique<GraphVideoSender>(graph, [&](const auto& frame) { triggers.push(frame); });
  auto sender2 = std::make_unique<GraphVideoSender>(graph, [&](const auto& frame) { triggers2.push(frame); });
  check(has_edge(graph, "VideoEncodeInputNode", "VideoEncoderNode", "RawVideoFrame") &&
        has_edge(graph, "VideoEncoderNode", "VideoTrackInputNode", "EncodedVideoFrame"),
        "GraphEngine connects raw input, real encoder and native track output");
  check(has_edge(graph, "RtcDecodeInputNode", "VideoDecoderNode", "VideoDecodeRequest") &&
        has_edge(graph, "VideoDecoderNode", "RtcDecodeOutputNode", "DecodedVideoFrame"),
        "DecoderFactory creates an executable GraphEngine decoder pipeline");
  auto push = [&](auto& source) {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    auto frame = video_frame(); frame.set_timestamp_us(env.clock().CurrentTime().us()); source->submit(frame);
  };
  std::vector<VideoFrameType> delta{VideoFrameType::kVideoFrameDelta}, key{VideoFrameType::kVideoFrameKey};
  // Probe obtains the negotiated codec, bound by source identity in its buffer.
  push(sender); auto probe = triggers.take();
  check(encoder->Encode(probe, &key) == 0 && encoded.count == 0, "probe never transmits a fake picture");
  push(sender2); auto probe2 = triggers2.take(); encoder2->Encode(probe2, &key);
  push(sender); auto packet = triggers.take();
  auto nodes = graph->snapshot();
  check(std::any_of(nodes.begin(), nodes.end(), [](const auto& n) { return n.kind == "VideoEncoderNode" && n.outputs == 1; }),
        "real encoding completes before WebRTC invokes passthrough Encode");
  check(triggers.producer != std::this_thread::get_id(), "real codec runs on graph sequence");
  check(encoded.count == 0 && encoded2.count == 0, "no packet forwarded before matching trigger");
  check(encoder->Encode(packet, &delta) == 0 && encoded.count == 1, "matching encoded packet forwarded");
  check(encoder->Encode(packet, &delta) == 0 && encoded.count == 1, "cadence repeat never replays an encoded reference picture");
  check(encoded.frame->_frameType == VideoFrameType::kVideoFrameKey, "first real frame is keyframe");
  check(encoded2.count == 0, "same-format tracks never cross-bind");
  push(sender2); encoder2->Encode(triggers2.take(), &delta);
  check(encoded2.count == 1, "second sender independently encoded");
  VideoDecoder::Settings ds; ds.set_codec_type(kVideoCodecVP8); ds.set_number_of_cores(1);
  DecodedFrames decoded; check(decoder->Configure(ds), "decoder configure"); decoder->RegisterDecodeCompleteCallback(&decoded);
  check(decoder->Decode(*encoded.frame, 0) == 0 && decoded.count == 1, "graph passthrough produces real decodable video");
  // Discard a trigger as WebRTC may do under load. No FIFO packet can leak into
  // the next trigger, and the reference chain must recover with a keyframe.
  push(sender); { auto dropped = triggers.take(); }
  push(sender); auto recovery = triggers.take();
  encoder->Encode(recovery, &delta);
  check(encoded.count == 2 && encoded.frame->_frameType == VideoFrameType::kVideoFrameKey,
        "discarded encoded trigger requests a fresh graph keyframe");
  check(encoded.frame->RtpTimestamp() == recovery.rtp_timestamp(), "packet and trigger RTP timestamps match");
  push(sender); encoder->Encode(triggers.take(), &key);
  push(sender); encoder->Encode(triggers.take(), &delta);
  check(encoded.frame->_frameType == VideoFrameType::kVideoFrameKey, "PLI reaches actual graph encoder");
  const int before_mute = encoded.count;
  check(encoder->Encode(video_frame(), &delta) == 0 && encoded.count == before_mute,
        "stock black-frame mute never leaks source packets or runs the real codec");
  push(sender); encoder->Encode(triggers.take(), &delta);
  check(encoded.count == before_mute + 1 && encoded.frame->_frameType == VideoFrameType::kVideoFrameKey,
        "unmute recovers with a graph keyframe");
  // Keep delegate factory leases through graph codec reconfiguration.
  deps.video_encoder_factory.reset();
  encoder->Release(); encoder->Release(); encoder->InitEncode(&config, settings);
  encoder->RegisterEncodeCompleteCallback(&encoded);
  push(sender); encoder->Encode(triggers.take(), &delta);
  check(encoded.frame->_frameType == VideoFrameType::kVideoFrameKey, "reinitialize after factory destruction");
  sender->pause(); sender->resume();
  push(sender); encoder->Encode(triggers.take(), &delta); // new source binding probe
  push(sender); encoder->Encode(triggers.take(), &delta);
  check(encoded.frame->_frameType == VideoFrameType::kVideoFrameKey, "source pause/resume rebuilds codec safely");
  sender->pause(); sender2->pause();
  sender.reset(); sender2.reset();
  encoder.reset(); encoder2.reset(); decoder->Release(); decoder.reset();
  check(graph->snapshot().empty(), "native envelope weak leases do not retain graph nodes");
}
static void graph_encode_queues() {
  auto env = CreateEnvironment(); EncodeExecutor executor(env, "graph-queue-test", 1);
  std::promise<void> entered, unblock; auto release = unblock.get_future().share();
  std::atomic<int> processed = 0, control = 0;
  auto entered_future = entered.get_future();
  executor.post([&] { entered.set_value(); release.wait(); ++processed; }, true);
  auto ready = entered_future.wait_for(std::chrono::seconds(5));
  if (ready != std::future_status::ready) unblock.set_value();
  check(ready == std::future_status::ready, "graph worker started");
  size_t dropped = 0;
  for (int i = 0; i < 20; ++i) dropped += executor.post([&] { ++processed; }, true);
  executor.post([&] { ++control; });
  unblock.set_value(); executor.flush();
  check(processed == 2 && dropped == 19 && control == 1, "graph bounds raw frames without dropping control messages");
}
static void channels() {
  RealtimeChannel<int> latest(2, 8, true);
  check(latest.push(1, 4, 0) && latest.push(2, 4, 0) && latest.push(3, 4, 0), "latest input");
  check(latest.dropped() == 1 && latest.wait_pop() == 2, "bounded latest drops oldest");
  latest.reset(1); check(!latest.push(4, 4, 0), "old generation rejected");
  check(!latest.push(4, 9, 1), "oversize rejected"); latest.close(); check(!latest.wait_pop(), "close unblocks wait");
  RealtimeChannel<int> reliable(1, 4, false);
  check(reliable.push(1, 4, 0) && !reliable.push(2, 4, 0) && reliable.wait_pop() == 1, "non-droppable branch rejects new");
  std::thread consumer([&] { check(!reliable.wait_pop(), "blocked consumer released"); });
  reliable.close(); consumer.join();
}
class TestAudioDevice : public lk::HeadlessAudioDevice {
 public:
  AudioTransport* callback = nullptr;
  int32_t RegisterAudioCallback(AudioTransport* transport) override {
    auto result = HeadlessAudioDevice::RegisterAudioCallback(transport); callback = transport; return result;
  }
};
class TestAudioTransport final : public AudioTransport {
 public:
  int captured = 0; std::atomic<int> rendered{0}; std::optional<int64_t> timestamp;
  int32_t RecordedDataIsAvailable(const void*, size_t samples, size_t bytes, size_t channels,
      uint32_t rate, uint32_t delay, int32_t drift, uint32_t level, bool key, uint32_t& new_level) override {
    check(samples == 480 && bytes == 2 && channels == 1 && rate == 48000 && delay == 7 && drift == 0 && !key, "ADM capture arguments");
    new_level = level + 1; ++captured; return 0;
  }
  int32_t RecordedDataIsAvailable(const void* data, size_t samples, size_t bytes, size_t channels,
      uint32_t rate, uint32_t delay, int32_t drift, uint32_t level, bool key, uint32_t& new_level,
      std::optional<int64_t> capture_time) override {
    timestamp = capture_time;
    return RecordedDataIsAvailable(data, samples, bytes, channels, rate, delay, drift, level, key, new_level);
  }
  int32_t NeedMorePlayData(size_t samples, size_t bytes, size_t channels, uint32_t,
      void* data, size_t& count, int64_t* elapsed, int64_t* ntp) override {
    std::memset(data, 0, samples * bytes * channels); count = samples;
    *elapsed = 1; *ntp = 2; ++rendered; return 0;
  }
  void PullRenderData(int bits, int, size_t channels, size_t frames, void* data, int64_t* elapsed, int64_t* ntp) override {
    std::memset(data, 0, frames * channels * bits / 8); *elapsed = 1; *ntp = 2; ++rendered;
  }
};
static void audio_device() {
  auto graph = std::make_shared<RtcGraph>(); auto device = make_ref_counted<TestAudioDevice>();
  auto bridge = make_ref_counted<GraphAudioDevice>(device, graph);
  TestAudioTransport transport;
  bridge->Init(); bridge->RegisterAudioCallback(&transport);
  std::array<int16_t, 480> pcm{}; uint32_t level = 0;
  check(device->callback->RecordedDataIsAvailable(pcm.data(), 480, 2, 1, 48000, 7, 0, 10, false, level, 123456) == 0,
        "capture callback bridged");
  check(level == 11 && transport.timestamp == 123456 && transport.captured == 1, "ADM mic feedback and capture timestamp");
  bridge->InitPlayout(); bridge->StartPlayout();
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (transport.rendered < 3 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  bridge->StopPlayout();
  auto nodes = graph->snapshot();
  check(nodes.size() == 2 && nodes[0].inputs == 1 && nodes[1].outputs >= 3, "separate device input/output nodes");
  bridge->Terminate(); check(device->callback == nullptr, "device callback detached before release");
  bridge = nullptr; check(graph->snapshot().empty(), "device node leases released");
}
static void source_and_render() {
  auto graph = std::make_shared<RtcGraph>();
  int callbacks = 0;
  lk::VideoFrames output([&](const auto& frame) { check(frame.width == 160 && frame.height == 120, "preview pixels"); ++callbacks; });
  livekit::sdk::MediaSpec spec; spec.demo = true; spec.width = 160; spec.height = 120; spec.local_preview = true;
  auto capture = std::make_shared<lk::MediaPump>(spec, graph, nullptr, &output, "test");
  capture->start();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (capture->frames < 4 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  output.dispatch(); check(callbacks > 0, "local preview branch works without a sender");
  capture->stop(); check(capture->error().empty(), "capture succeeded");
  auto nodes = graph->snapshot(); check(nodes.size() == 5, "capture/input/encoder/publish/render are separate engine nodes");
  check(has_edge(graph, "VideoSourceNode", "VideoEncodeInputNode", "RawVideoFrame") &&
        has_edge(graph, "VideoSourceNode", "VideoRenderNode", "RawVideoFrame"), "capture fan-out uses graph edges");
  output.dispatch(); check(callbacks == 1, "no late preview after stop");
  const auto previous_frames = capture->frames.load();
  check(capture->start(), "restart capture graph");
  const auto restart_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (capture->frames == previous_frames && std::chrono::steady_clock::now() < restart_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  output.dispatch(); check(callbacks == 2, "GraphEngine restart preserves preview subscription");
  capture->stop();
  capture.reset(); check(graph->snapshot().empty(), "source graph cleanup");
  auto camera = livekit::sdk::probe_media("camera://0", "not-installed");
  auto screen = livekit::sdk::probe_media("screen://primary", "not-installed");
  check(camera[0].capture == livekit::sdk::CaptureKind::camera && screen[0].capture == livekit::sdk::CaptureKind::screen, "native sources bypass FFmpeg");
}
static void engine_routing() {
  auto graph = std::make_shared<RtcGraph>();
  auto source = std::make_shared<MediaEventNode<RawVideoFrame>>("source", NodeExecution::source);
  int left_count = 0, right_count = 0;
  auto left = std::make_shared<MediaEventNode<RawVideoFrame>>("left", NodeExecution::sink,
      [&](auto& event) { check(event.frame.rtp_timestamp() == 9000, "edge preserves frame timestamp"); ++left_count; });
  auto right = std::make_shared<MediaEventNode<RawVideoFrame>>("right", NodeExecution::sink,
      [&](auto& event) { check(event.frame.video_frame_buffer()->ToI420()->DataY()[0] == 100, "edge preserves pixels"); ++right_count; });
  RtcPipeline pipeline(graph);
  pipeline.build({source, left, right}, {RtcPipeline::connect<RawVideoFrame>(source, left),
                                       RtcPipeline::connect<RawVideoFrame>(source, right)});
  check(source->state() == graphflow::core::LifecycleState::Initialized, "engine owns initialization");
  check(pipeline.start(), "engine start");
  source->emit(RawVideoFrame(video_frame()));
  check(left_count == 1 && right_count == 1, "typed edges fan out to both subscribers");
  pipeline.stop(); source->emit(RawVideoFrame(video_frame()));
  check(left_count == 1 && right_count == 1, "stopped graph rejects input");
  check(pipeline.start(), "engine restart"); source->emit(RawVideoFrame(video_frame()));
  check(left_count == 2 && right_count == 2, "restart does not duplicate subscriptions");
  pipeline.release();
  check(left->state() == graphflow::core::LifecycleState::Released &&
        source->state() == graphflow::core::LifecycleState::Released, "engine releases all nodes");
  source->emit(RawVideoFrame(video_frame()));
  check(left_count == 2 && right_count == 2, "released ingress never accesses destroyed edge buses");
  auto bad = std::make_shared<MediaEventNode<RawVideoFrame>>("bad", NodeExecution::source);
  bool rejected = false;
  try { RtcPipeline invalid(graph); invalid.build({bad}, {{bad->getName(), "missing", "RawVideoFrame", {}}}); }
  catch (const std::runtime_error&) { rejected = true; }
  check(rejected, "invalid endpoint is rejected by GraphEngine");
}
static void recording() {
  auto directory = std::filesystem::temp_directory_path() / ("livekit-graph-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  check(std::filesystem::create_directory(directory), "unique recording test directory");
  {
    lk::RecordingNode node(directory, "video", true); node.init(); node.start();
    node.submit(video_frame()); node.submit(video_frame(12000)); node.stop();
    check(node.error().empty() && node.outputs == 2, "recording drains video branch");
    check(std::filesystem::file_size(directory / "video-0.y4m") > 160 * 120 * 3, "actual Y4M written");
    node.start(); node.submit(video_frame()); node.stop();
    check(node.error().empty() && node.outputs == 3 && std::filesystem::exists(directory / "video-1.y4m"),
          "recording queue reopens on graph restart");
  }
  {
    lk::RecordingNode node(directory, "audio", false); node.init(); node.start();
    std::array<int16_t, 480> pcm{}; node.submit(pcm.data(), 16, 48000, 1, 480); node.stop();
    check(node.error().empty() && std::filesystem::file_size(directory / "audio-0.wav") == 1004, "WAV header finalized on drain");
    node.start(); node.submit(pcm.data(), 16, 48000, 1, 480); node.stop();
    check(node.error().empty() && std::filesystem::file_size(directory / "audio-1.wav") == 1004,
          "restarted audio recording preserves the previous segment");
  }
  std::filesystem::remove_all(directory);
}
int main() {
  try { engine_routing(); channels(); audio_codecs(); graph_driven_video(); graph_encode_queues(); audio_device(); source_and_render(); recording();
    std::cout << "media graph codec/source/render/channel tests passed\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
