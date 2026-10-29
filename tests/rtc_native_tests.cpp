#include "backends/rtc/encoded_audio_sender.hpp"
#include "backends/rtc/encoded_receiver.hpp"
#include "backends/rtc/encoded_video_sender.hpp"
#include "backends/rtc/transport_runtime.hpp"
#include "backends/rtc/native_peer.hpp"
#include "backends/rtc/queue_budget.hpp"
#include "rtc_video_fixtures.hpp"
#include "api/make_ref_counted.h"
#include "api/rtp_transceiver_interface.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/environment/environment_factory.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "audio/channel_send_frame_transformer_delegate.h"
#include "rtc_base/logging.h"
#include <atomic>
#include <future>
#include <iostream>
#include <map>
#include <thread>

using namespace livekit::sdk::rtc;
namespace native = livekit::sdk::rtc::detail;

namespace {
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
template<class T> void ready(std::future<T>& future) {
  check(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "Native task queue timed out");
}
void drain(webrtc::TaskQueueBase* queue) {
  auto barrier = std::make_shared<std::promise<void>>();
  auto done = barrier->get_future();
  queue->PostTask([barrier] { barrier->set_value(); });
  ready(done);
}
class QueueGate {
 public:
  explicit QueueGate(webrtc::TaskQueueBase* queue) {
    auto entered = std::make_shared<std::promise<void>>();
    auto pending = entered->get_future();
    auto release = release_.get_future().share();
    queue->PostTask([entered, release] { entered->set_value(); release.wait(); });
    ready(pending);
  }
  ~QueueGate() { release_.set_value(); }
 private:
  std::promise<void> release_;
};
EncodedFrame sample(uint64_t timestamp = 0xfffffff0ULL) {
  EncodedFrame frame;
  frame.buffer = EncodedBuffer({0xf8, 0xff, 0xfe});
  frame.epoch = 7;
  frame.timestamp = timestamp;
  frame.duration = std::chrono::milliseconds(20);
  return frame;
}

void external_audio() {
  auto factory = webrtc::CreateDefaultTaskQueueFactory();
  auto queue = factory->CreateTaskQueue("ExternalAudioTest", webrtc::TaskQueueFactory::Priority::NORMAL);
  QueueLimits limits;
  limits.max_frames = 2;
  limits.max_bytes = 6;
  auto sender = webrtc::make_ref_counted<native::EncodedAudioSender>(
      native::BindingToken{"audio", 7, 1, true}, limits);
  auto frame = sample();
  check(sender->send(frame) == native::SendResult::would_block, "Unnegotiated sender must wait");
  sender->configure(1234, 111);
  std::atomic<int> sent{0};
  std::atomic<bool> valid{true};
  auto delegate = webrtc::make_ref_counted<webrtc::ChannelSendFrameTransformerDelegate>(
      [&](webrtc::AudioFrameType kind, uint8_t pt, uint32_t timestamp,
          webrtc::ArrayView<const uint8_t> payload, int64_t capture,
          webrtc::ArrayView<const uint32_t> csrcs, std::optional<uint8_t> level) {
        if (kind != webrtc::AudioFrameType::kAudioFrameSpeech || pt != 111 ||
            (timestamp != 0xfffffff0u && timestamp != 944u) ||
            payload.size() != 3 || payload[0] != 0xf8 || payload[2] != 0xfe ||
            capture != 0 || !csrcs.empty() || level)
          valid = false;
        ++sent;
        return 0;
      }, sender, queue.get());
  delegate->Init(); // No Transform(), PCM input, codec or timer.
  {
    QueueGate gate(queue.get());
    check(sender->send(frame) == native::SendResult::sent, "First encoded frame rejected");
    check(sender->send(sample(0x1000003b0ULL)) == native::SendResult::sent, "Wrapped timestamp rejected");
    check(sender->send(frame) == native::SendResult::would_block, "Native queue not bounded");
    check(sender->outstanding_frames() == 2 && sender->outstanding_bytes() == 6,
          "Native queue reservation missing");
    check(sent == 0, "Queue gate failed");
  }
  drain(queue.get());
  check(sent == 2 && valid, "Native delegate did not preserve encoded audio");
  check(sender->outstanding_frames() == 0 && sender->outstanding_bytes() == 0,
        "Native completion did not release budget");
  {
    QueueGate gate(queue.get());
    check(sender->send(frame) == native::SendResult::sent, "Expiry setup failed");
    std::this_thread::sleep_for(limits.max_age + std::chrono::milliseconds(10));
  }
  drain(queue.get());
  check(sent == 2 && sender->outstanding_frames() == 0,
        "Native sender transmitted expired audio or leaked its reservation");
  frame.epoch = 6;
  check(sender->send(frame) == native::SendResult::dropped, "Old epoch reached native sender");
  frame.epoch = 7;
  {
    QueueGate gate(queue.get());
    check(sender->send(frame) == native::SendResult::sent, "Mute queue setup failed");
    sender->set_muted(true);
    check(sender->send(frame) == native::SendResult::dropped, "Muted native audio admitted input");
    sender->set_muted(false);
    check(sender->send(frame) == native::SendResult::sent, "Unmuted native audio rejected input");
  }
  drain(queue.get());
  check(sent == 3 && sender->outstanding_frames() == 0, "Old audio queue revived after quick unmute");
  {
    QueueGate gate(queue.get());
    check(sender->send(frame) == native::SendResult::sent, "Pending close setup failed");
    sender->close();
    check(sender->send(frame) == native::SendResult::dropped, "Closed sender accepted frame");
  }
  drain(queue.get());
  check(sent == 3, "Native queue sent a pending frame after source close");
  check(sender->outstanding_frames() == 0, "Source close leaked frame reservation");
  delegate->Reset();
}

void queued_video_mute() {
  struct Sink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    std::vector<webrtc::VideoFrame> frames;
    void OnFrame(const webrtc::VideoFrame& frame) override { frames.push_back(frame); }
  } sink;
  struct Output : webrtc::EncodedImageCallback {
    int frames = 0;
    Result OnEncodedImage(const webrtc::EncodedImage&, const webrtc::CodecSpecificInfo*) override {
      ++frames; return Result(Result::OK);
    }
  } output;
  TrackDescriptor descriptor; descriptor.id = "queued-video"; descriptor.format.codec = Codec::vp8;
  descriptor.format.width = 64; descriptor.format.height = 48;
  auto sender = webrtc::make_ref_counted<native::EncodedVideoSender>(native::BindingToken{descriptor.id, 7, 1, true}, descriptor);
  sender->AddOrUpdateSink(&sink, {});
  auto factory = native::create_transport_video_encoder_factory();
  auto encoder = factory->Create(webrtc::CreateEnvironment(), webrtc::SdpVideoFormat("VP8"));
  webrtc::VideoCodec codec; codec.codecType = webrtc::kVideoCodecVP8; codec.width = 64; codec.height = 48;
  check(encoder->InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)) == 0,
        "Packet envelope setup failed");
  encoder->RegisterEncodeCompleteCallback(&output);
  auto frame = sample(1000); frame.buffer = EncodedBuffer(fixture_video(Codec::vp8, false)); frame.keyframe = true;
  check(sender->send(frame) == native::SendResult::sent && sink.frames.size() == 1, "Video queue setup failed");
  sender->set_muted(true);
  check(sender->send(frame) == native::SendResult::dropped && !sender->feedback().encodings.at("").active,
        "Muted native video admitted media");
  sender->set_muted(false);
  encoder->Encode(sink.frames.front(), nullptr);
  check(output.frames == 0 && sender->feedback().encodings.at("").keyframe_required, "Queued video revived after unmute");
  frame.timestamp = 2000;
  check(sender->send(frame) == native::SendResult::sent, "Video unmute admission failed");
  encoder->Encode(sink.frames.back(), nullptr);
  check(output.frames == 1, "New video keyframe did not recover after unmute");
  frame.timestamp = 3000;
  check(sender->send(frame) == native::SendResult::sent, "Demand queue setup failed");
  sender->set_demand(false);
  check(!sender->feedback().encodings.at("").active && sender->send(frame) == native::SendResult::dropped,
        "Aggregate OFF did not close transport admission");
  sender->set_muted(true); sender->set_demand(true);
  check(!sender->feedback().encodings.at("").active, "Demand ON bypassed independent mute");
  sender->set_muted(false);
  encoder->Encode(sink.frames.back(), nullptr);
  check(output.frames == 1, "Queued video crossed demand OFF/ON boundary");
  frame.timestamp = 4000;
  check(sender->send(frame) == native::SendResult::sent, "Demand resume admission failed");
  encoder->Encode(sink.frames.back(), nullptr);
  check(output.frames == 2, "Demand resume did not send new keyframe");
  sink.frames.clear(); sender->RemoveSink(&sink); encoder->Release(); sender->close();
}

void shared_audio_budget() {
  auto factory = webrtc::CreateDefaultTaskQueueFactory();
  auto queue = factory->CreateTaskQueue("RoomBudgetTest", webrtc::TaskQueueFactory::Priority::NORMAL);
  auto room_budget = std::make_shared<native::QueueBudget>(1024);
  auto budget = std::make_shared<native::QueueBudget>(3, 1, room_budget);
  auto sender = webrtc::make_ref_counted<native::EncodedAudioSender>(native::BindingToken{"audio", 7, 1, true}, QueueLimits{});
  sender->configure(1234, 111);
  auto delegate = webrtc::make_ref_counted<webrtc::ChannelSendFrameTransformerDelegate>(
      [](webrtc::AudioFrameType, uint8_t, uint32_t, webrtc::ArrayView<const uint8_t>, int64_t,
         webrtc::ArrayView<const uint32_t>, std::optional<uint8_t>) { return 0; }, sender, queue.get());
  delegate->Init();
  {
    QueueGate gate(queue.get());
    {
      auto frame = sample();
      check(budget->charge(frame.buffer), "Failed to reserve room payload");
      check(sender->send(frame) == native::SendResult::sent, "Failed to transfer charged audio");
    }
    check(budget->used == 3, "Native sender released shared budget before transmission");
    check(budget->frames == 1 && room_budget->used == 3, "Native sender lost combined track/room accounting");
    auto blocked = sample();
    check(!budget->charge(blocked.buffer), "Native queue exceeded shared room budget");
    auto sibling = sample();
    auto sibling_budget = std::make_shared<native::QueueBudget>(3, 1, room_budget);
    check(sibling_budget->charge(sibling.buffer) && room_budget->used == 6,
          "One track's occupied native queue blocked an independent track");
  }
  drain(queue.get());
  check(budget->used == 0 && budget->frames == 0 && room_budget->used == 0,
        "Native completion leaked shared track/room budget");
  sender->close(); delegate->Reset();
}

void audio_frame_ownership() {
  int released = 0;
  std::vector<uint8_t> payload{1, 2, 3};
  auto frame = webrtc::CreateSenderAudioFrame(payload, 111, 123, 456, "audio/opus",
                                             1000, 42, [&] { ++released; });
  payload[0] = 9;
  check(frame->GetData()[0] == 1 && frame->GetSsrc() == 456 &&
        frame->AbsoluteCaptureTimestamp() == 1000 && frame->AudioLevel() == 42,
        "Factory must own payload and metadata");
  auto clone = webrtc::CloneSenderAudioFrame(frame.get());
  frame.reset();
  check(released == 1 && clone->GetData()[0] == 1, "Clone must have independent ownership");
  clone.reset();
  check(released == 1, "Clone duplicated release callback");
}

using PC = webrtc::PeerConnectionInterface;
// The fixture only waits for bundled ICE. SDP, remote track routing and PLI
// exercise the production NativePeer instead of a second implementation.
class LoopbackPeer : public native::NativePeer {
 public:
  explicit LoopbackPeer(native::TransportRuntime& runtime)
      : NativePeer(runtime, PC::RTCConfiguration{}, receive_limits()), runtime_(runtime) {}
  std::string local(bool offer) {
    auto created = create_local(offer);
    ready(created);
    created.get();
    refresh_receivers();
    const auto deadline = SteadyClock::now() + std::chrono::seconds(5);
    while (!gathered && SteadyClock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(gathered, "Loopback ICE gathering timed out");
    return runtime_.signaling->BlockingCall([&] {
      std::string sdp;
      pc->local_description()->ToString(&sdp);
      return sdp;
    });
  }
  void remote(webrtc::SdpType type, const std::string& sdp) {
    auto completion = set_remote({type, sdp});
    ready(completion);
    completion.get();
    refresh_receivers();
  }
  std::map<std::string, webrtc::scoped_refptr<native::EncodedReceiver>> outputs() {
    std::map<std::string, webrtc::scoped_refptr<native::EncodedReceiver>> result;
    for (const auto& [sid, track] : remote_tracks()) result.emplace(sid, track.output);
    return result;
  }
  void keyframe(const std::string& sid) { request_keyframe(sid); }
 private:
  static QueueLimits receive_limits() {
    QueueLimits limits;
    limits.max_age = std::chrono::seconds(5);
    return limits;
  }
  native::TransportRuntime& runtime_;
};
void audio_loopback() {
  native::TransportRuntime runtime;
  LoopbackPeer publisher(runtime), subscriber(runtime);
  std::vector<webrtc::scoped_refptr<native::EncodedAudioSender>> inputs;
  std::vector<webrtc::scoped_refptr<webrtc::RtpSenderInterface>> senders;
  for (const auto* id : {"microphone", "music"}) {
    auto input = webrtc::make_ref_counted<native::EncodedAudioSender>(native::BindingToken{id, 7, 1, true}, QueueLimits{});
    auto track = runtime.factory->CreateAudioTrack(id, nullptr);
    auto added = publisher.pc->AddTrack(track, {id});
    check(added.ok(), "Cannot add audio sender");
    auto sender = added.MoveValue();
    sender->SetEncoderToPacketizerFrameTransformer(input);
    inputs.push_back(input);
    senders.push_back(sender);
  }
  subscriber.remote(webrtc::SdpType::kOffer, publisher.local(true));
  publisher.remote(webrtc::SdpType::kAnswer, subscriber.local(false));
  auto deadline = SteadyClock::now() + std::chrono::seconds(5);
  while ((!publisher.connected || !subscriber.connected) && SteadyClock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  check(publisher.connected && subscriber.connected, "Loopback ICE/DTLS failed");
  for (size_t i = 0; i < senders.size(); ++i) {
    auto parameters = senders[i]->GetParameters();
    check(parameters.encodings.size() == 1 && parameters.encodings[0].ssrc.has_value(), "Missing negotiated audio SSRC");
    auto codec = std::find_if(parameters.codecs.begin(), parameters.codecs.end(), [](const auto& codec) { return codec.name == "opus"; });
    check(codec != parameters.codecs.end(), "Missing negotiated Opus payload type");
    inputs[i]->configure(*parameters.encodings[0].ssrc, uint8_t(codec->payload_type));
  }
  auto outputs = subscriber.outputs();
  check(outputs.size() == 2 && outputs.contains("microphone") && outputs.contains("music"), "Audio tracks not independently routed");
  std::map<std::string, std::vector<native::ReceivedAccessUnit>> received;
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto frame = sample(100000 + i * 48000);
    frame.buffer = EncodedBuffer({0xf8, 0xff, uint8_t(0xe0 + i)});
    check(inputs[i]->send(frame) == native::SendResult::sent, "Negotiated external audio send failed");
  }
  deadline = SteadyClock::now() + std::chrono::seconds(5);
  while ((received["microphone"].empty() || received["music"].empty()) && SteadyClock::now() < deadline) {
    for (auto& [id, output] : outputs) {
      auto frames = output->poll();
      for (auto& frame : frames) received[id].push_back(std::move(frame));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto& frames = received[i == 0 ? "microphone" : "music"];
    check(frames.size() == 1, "Encoded audio did not cross SRTP transport exactly once");
    check(frames[0].buffer.bytes().size() == 3 && frames[0].buffer.bytes()[2] == 0xe0 + i,
          "Encoded audio payload crossed track boundary or was modified");
    check(frames[0].timestamp == 100000 + i * 48000, "SRTP changed media timestamp");
  }
  inputs[0]->close();
  check(publisher.pc->RemoveTrackOrError(senders[0]).ok(), "Independent audio removal failed");
  check(inputs[1]->send(sample(200000)) == native::SendResult::sent, "Sibling sender closed with removed track");
  deadline = SteadyClock::now() + std::chrono::seconds(5);
  std::vector<native::ReceivedAccessUnit> sibling;
  while (sibling.empty() && SteadyClock::now() < deadline) {
    sibling = outputs["music"]->poll();
    if (sibling.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  check(!sibling.empty() && sibling[0].timestamp == 200000, "Sibling transport stopped after track removal");
  // A server can reuse a native receiver while changing its publication SID.
  // Resolve the server MID mapping instead of keeping the original track id.
  std::string music_mid;
  for (const auto& transceiver : subscriber.pc->GetTransceivers())
    if (transceiver->receiver()->track()->id() == "music") music_mid = transceiver->mid().value_or("");
  check(!music_mid.empty(), "Missing music MID");
  subscriber.take_track_events();
  auto remapped = subscriber.set_remote({webrtc::SdpType::kOffer, publisher.local(true)}, {{music_mid, "TR_music"}});
  ready(remapped); remapped.get();
  publisher.remote(webrtc::SdpType::kAnswer, subscriber.local(false));
  auto remapped_outputs = subscriber.outputs();
  check(remapped_outputs.size() == 1 && remapped_outputs.contains("TR_music"),
        "Receiver rebind retained an old SID or resurrected a removed track");
  const auto changes = subscriber.take_track_events();
  check(std::any_of(changes.begin(), changes.end(), [](const auto& event) { return !event.added && event.sid == "music"; }) &&
        std::any_of(changes.begin(), changes.end(), [](const auto& event) { return event.added && event.sid == "TR_music"; }),
        "Receiver rebind did not emit removal/addition");
  check(inputs[1]->send(sample(210000)) == native::SendResult::sent, "Rebound audio send failed");
  deadline = SteadyClock::now() + std::chrono::seconds(5);
  std::vector<native::ReceivedAccessUnit> rebound;
  while (rebound.empty() && SteadyClock::now() < deadline) {
    rebound = remapped_outputs.at("TR_music")->poll();
    if (rebound.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  check(rebound.size() == 1 && rebound[0].timestamp == 210000, "Reused receiver did not route encoded audio to the new SID");
  check(runtime.codec_counters->pcm_encode_calls == 0 && runtime.codec_counters->pcm_decode_calls == 0,
        "Encoded transport used PCM encode/decode path");
  for (auto& input : inputs) input->close();
}

void pending_native_close() {
  native::TransportRuntime runtime;
  native::NativePeer peer(runtime, PC::RTCConfiguration{});
  auto statistics=peer.statistics(); ready(statistics);
  const auto report=statistics.get();
  check(report.timestamp_us>0 && report.json.starts_with('[') && report.json.ends_with(']') &&
    report.json.find("local-candidate")==std::string::npos && report.json.find("remote-candidate")==std::string::npos &&
    report.json.find("certificate")==std::string::npos,"Native diagnostics exposed non-media records");
  auto channel = peer.pc->CreateDataChannelOrError("_test", nullptr);
  check(channel.ok(), "Cannot create negotiation fixture channel");
  auto pending = peer.create_local(true);
  peer.close();
  ready(pending);
  try { pending.get(); } catch (const std::runtime_error&) {}
  auto after_close = peer.create_local(true);
  ready(after_close);
  bool rejected = false;
  try { after_close.get(); } catch (const std::runtime_error&) { rejected = true; }
  check(rejected, "Closed native peer accepted negotiation");
  auto stopped_stats=peer.statistics(); ready(stopped_stats);
  rejected=false; try { stopped_stats.get(); } catch(const std::runtime_error&) { rejected=true; }
  check(rejected,"Closed native peer accepted statistics request");
}

std::vector<uint8_t> canonical_annex_b(std::span<const uint8_t> input) {
  std::vector<uint8_t> result;
  for (size_t i = 0; i < input.size();) {
    if (i + 4 <= input.size() && input[i] == 0 && input[i+1] == 0 && input[i+2] == 0 && input[i+3] == 1) {
      result.insert(result.end(), {0, 0, 0, 1}); i += 4;
    } else if (i + 3 <= input.size() && input[i] == 0 && input[i+1] == 0 && input[i+2] == 1) {
      result.insert(result.end(), {0, 0, 0, 1}); i += 3;
    } else result.push_back(input[i++]);
  }
  return result;
}
void video_loopback(Codec codec) {
  native::TransportRuntime runtime;
  LoopbackPeer publisher(runtime), subscriber(runtime);
  std::vector<webrtc::scoped_refptr<native::EncodedVideoSender>> inputs;
  for (const auto* id : {"camera", "screen"}) {
    TrackDescriptor descriptor;
    descriptor.id = id;
    descriptor.format.codec = codec;
    descriptor.format.width = 64;
    descriptor.format.height = 48;
    auto input = runtime.signaling->BlockingCall([&] {
      return webrtc::make_ref_counted<native::EncodedVideoSender>(native::BindingToken{id, 7, 1, true}, descriptor);
    });
    auto track = runtime.factory->CreateVideoTrack(input, id);
    auto added = publisher.pc->AddTrack(track, {id});
    check(added.ok(), "Cannot add encoded video sender");
    const std::string codec_name = codec == Codec::vp8 ? "VP8" : codec == Codec::h264 ? "H264" : "H265";
    std::vector<webrtc::RtpCodecCapability> selected;
    for (const auto& capability : runtime.factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO).codecs)
      if (capability.name == codec_name) selected.push_back(capability);
    check(!selected.empty(), "Missing native encoded video capability");
    for (const auto& transceiver : publisher.pc->GetTransceivers())
      if (transceiver->sender() == added.value())
        check(transceiver->SetCodecPreferences(selected).ok(), "Cannot select external video codec");
    inputs.push_back(input);
  }
  subscriber.remote(webrtc::SdpType::kOffer, publisher.local(true));
  publisher.remote(webrtc::SdpType::kAnswer, subscriber.local(false));
  auto deadline = SteadyClock::now() + std::chrono::seconds(5);
  while ((!publisher.connected || !subscriber.connected) && SteadyClock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  check(publisher.connected && subscriber.connected, "Video loopback ICE/DTLS failed");
  auto outputs = subscriber.outputs();
  check(outputs.contains("camera") && outputs.contains("screen"), "Missing independent video receivers");
  std::map<std::string, native::ReceivedAccessUnit> received;
  for (int attempt = 0; attempt < 40 && received.size() < 2; ++attempt) {
    for (size_t i = 0; i < inputs.size(); ++i) {
      const auto id = i == 0 ? "camera" : "screen";
      if (received.contains(id)) continue;
      auto frame = sample(100001 + i * 90000 + attempt * 3000);
      frame.buffer = EncodedBuffer(fixture_video(codec, i == 1));
      frame.keyframe = true;
      const auto result = inputs[i]->send(frame);
      check(result != native::SendResult::dropped, "Encoded video ingress rejected valid keyframe");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (auto& [id, output] : outputs) {
      auto frames = output->poll();
      if (!frames.empty()) received.insert_or_assign(id, std::move(frames.back()));
    }
  }
  check(received.size() == 2, "Encoded video did not cross SRTP transport");
  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto& frame = received.at(i == 0 ? "camera" : "screen");
    const auto bytes = frame.buffer.bytes();
    auto expected = fixture_video(codec, i == 1);
    auto actual = std::vector<uint8_t>(bytes.begin(), bytes.end());
    // H26x RTP carries NAL units; Annex-B delimiter lengths are not on wire.
    if (codec != Codec::vp8) { expected = canonical_annex_b(expected); actual = canonical_annex_b(actual); }
    if (actual != expected) {
      const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin(), expected.end());
      std::cerr << "codec=" << int(codec) << " actual=" << actual.size() << " expected=" << expected.size()
                << " first_difference=" << (mismatch.first - actual.begin()) << '\n';
    }
    check(actual == expected, "Video payload modified or routed to another track");
    check(frame.keyframe && frame.width == 64 && frame.height == 48, "Video access unit metadata lost");
  }
  // Native RTP randomizes the initial video timestamp. Check clock deltas,
  // including sub-millisecond precision, rather than assuming a wire origin.
  auto timed_frame = [&](uint64_t timestamp) {
    auto frame = sample(timestamp);
    frame.buffer = EncodedBuffer(fixture_video(codec, false));
    frame.keyframe = true;
    outputs.at("camera")->poll();
    check(inputs[0]->send(frame) == native::SendResult::sent, "Timed video frame rejected");
    const auto until = SteadyClock::now() + std::chrono::seconds(2);
    while (SteadyClock::now() < until) {
      auto frames = outputs.at("camera")->poll();
      if (!frames.empty()) return frames.back().timestamp;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Timed video frame missing");
  };
  const auto first_timestamp = timed_frame(1000001);
  std::this_thread::sleep_for(std::chrono::milliseconds(35));
  const auto second_timestamp = timed_frame(1003002);
  check(uint32_t(second_timestamp - first_timestamp) == 3001,
        "External video media-clock delta was quantized or remapped");
  const auto before = inputs[0]->feedback().encodings.begin()->second.keyframe_requests;
  subscriber.keyframe("camera");
  deadline = SteadyClock::now() + std::chrono::seconds(3);
  while (inputs[0]->feedback().encodings.begin()->second.keyframe_requests <= before && SteadyClock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  check(inputs[0]->feedback().encodings.begin()->second.keyframe_requests > before,
        "PLI feedback waited for another source frame");
  check(runtime.codec_counters->unexpected_video_decode_calls == 0,
        "Encoded video leaked into native decoder boundary");
  for (auto& input : inputs) input->close();
}
} // namespace

int main() {
  try {
    webrtc::LogMessage::LogToDebug(webrtc::LS_ERROR);
    shared_audio_budget();
    audio_frame_ownership();
    external_audio(); queued_video_mute();
    audio_loopback();
    for (const auto codec : {Codec::vp8, Codec::h264, Codec::h265}) video_loopback(codec);
    pending_native_close();
    std::cout << "Native encoded transport tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
