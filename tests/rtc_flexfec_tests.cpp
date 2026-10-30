#include "backends/webrtc/encoded_video_sender.hpp"
#include "backends/webrtc/native_peer.hpp"
#include "backends/webrtc/transport_runtime.hpp"
#include "livekit/rtc/livekit.hpp"
#include "rtc_video_fixtures.hpp"
#include "api/fec_controller.h"
#include "api/make_ref_counted.h"
#include "api/packet_socket_factory.h"
#include "p2p/base/basic_packet_socket_factory.h"
#include "rtc_base/logging.h"
#include <algorithm>
#include <atomic>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

using namespace livekit::sdk::rtc;
namespace native = livekit::sdk::rtc::detail;

namespace {
void check(bool value, const char* message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

template <class T>
T complete(std::future<T> pending) {
  check(pending.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "Native negotiation timed out");
  return pending.get();
}

// Observe SRTP headers and drop ciphertext before WebRTC authentication. Never
// edit SDP payloads, decrypted RTP, or the receiver's recovered-frame output.
struct LossState {
  std::mutex mutex;
  int media_pt = -1;
  std::set<int> media_pts;
  int fec_pt = -1;
  bool armed = false;
  bool discard_fec = false;
  bool outgoing = false;
  size_t media_packets = 0;
  size_t fec_packets = 0;
  size_t duplicate_drops = 0;
  std::map<uint16_t, uint32_t> dropped;
  std::set<uint32_t> delivered_timestamps;

  bool discard(webrtc::ArrayView<const uint8_t> packet) {
    if (packet.size() < 12 || (packet[0] >> 6) != 2) {
      return false;
    }
    std::lock_guard lock(mutex);
    const int pt = packet[1] & 0x7f;
    if (pt == fec_pt) {
      ++fec_packets;
      return discard_fec;
    }
    if ((pt != media_pt && !media_pts.contains(pt)) || (packet[0] & 0x20)) {
      return false;
    }
    ++media_packets;
    const auto sequence = uint16_t((uint16_t(packet[2]) << 8) | packet[3]);
    const auto timestamp = (uint32_t(packet[4]) << 24) | (uint32_t(packet[5]) << 16) |
                           (uint32_t(packet[6]) << 8) | packet[7];
    if (dropped.contains(sequence)) {
      ++duplicate_drops;
      return true;  // Same-SSRC NACK retransmission must not rescue the test.
    }
    if (armed && (packet[1] & 0x80) && media_packets % 10 == 0 && dropped.size() < 10) {
      dropped.emplace(sequence, timestamp);
      return true;
    }
    return false;
  }

  size_t recovered() const {
    return std::count_if(dropped.begin(), dropped.end(), [&](const auto& entry) {
      return delivered_timestamps.contains(entry.second);
    });
  }
};

class LossSocket final : public webrtc::AsyncPacketSocket {
 public:
  LossSocket(webrtc::AsyncPacketSocket* socket, std::shared_ptr<LossState> state)
      : socket_(socket), state_(std::move(state)) {
    socket_->RegisterReceivedPacketCallback(
        [this](webrtc::AsyncPacketSocket*, const webrtc::ReceivedIpPacket& packet) {
          if (state_->outgoing || !state_->discard(packet.payload())) {
            NotifyPacketReceived(packet);
          }
        });
    socket_->SignalSentPacket.connect(this, &LossSocket::sent);
    socket_->SignalReadyToSend.connect(this, &LossSocket::ready);
    socket_->SignalAddressReady.connect(this, &LossSocket::address_ready);
    socket_->SignalConnect.connect(this, &LossSocket::connected);
    socket_->SubscribeCloseEvent(
        this, [this](webrtc::AsyncPacketSocket*, int error) { NotifyClosed(error); });
  }

  ~LossSocket() override {
    socket_->DeregisterReceivedPacketCallback();
    socket_->UnsubscribeCloseEvent(this);
  }

  webrtc::SocketAddress GetLocalAddress() const override {
    return socket_->GetLocalAddress();
  }

  webrtc::SocketAddress GetRemoteAddress() const override {
    return socket_->GetRemoteAddress();
  }

  int Send(const void* data, size_t size,
           const webrtc::AsyncSocketPacketOptions& options) override {
    if (state_->outgoing && state_->discard({static_cast<const uint8_t*>(data), size})) {
      return static_cast<int>(size);
    }
    return socket_->Send(data, size, options);
  }

  int SendTo(const void* data, size_t size, const webrtc::SocketAddress& address,
             const webrtc::AsyncSocketPacketOptions& options) override {
    if (state_->outgoing && state_->discard({static_cast<const uint8_t*>(data), size})) {
      return static_cast<int>(size);
    }
    return socket_->SendTo(data, size, address, options);
  }

  int Close() override {
    return socket_->Close();
  }

  State GetState() const override {
    return socket_->GetState();
  }

  int GetOption(webrtc::Socket::Option option, int* value) override {
    return socket_->GetOption(option, value);
  }

  int SetOption(webrtc::Socket::Option option, int value) override {
    return socket_->SetOption(option, value);
  }

  int GetError() const override {
    return socket_->GetError();
  }

  void SetError(int error) override {
    socket_->SetError(error);
  }

 private:
  void sent(webrtc::AsyncPacketSocket*, const webrtc::SentPacketInfo& packet) {
    SignalSentPacket(this, packet);
  }

  void ready(webrtc::AsyncPacketSocket*) {
    SignalReadyToSend(this);
  }

  void address_ready(webrtc::AsyncPacketSocket*, const webrtc::SocketAddress& address) {
    SignalAddressReady(this, address);
  }

  void connected(webrtc::AsyncPacketSocket*) {
    SignalConnect(this);
  }

  std::unique_ptr<webrtc::AsyncPacketSocket> socket_;
  std::shared_ptr<LossState> state_;
};

class LossSocketFactory final : public webrtc::PacketSocketFactory {
 public:
  explicit LossSocketFactory(std::shared_ptr<LossState> state) : state_(std::move(state)) {}

  webrtc::AsyncPacketSocket* CreateUdpSocket(const webrtc::SocketAddress& address, uint16_t min,
                                             uint16_t max) override {
    auto* socket = factory().CreateUdpSocket(address, min, max);
    return socket ? new LossSocket(socket, state_) : nullptr;
  }

  webrtc::AsyncListenSocket* CreateServerTcpSocket(const webrtc::SocketAddress&, uint16_t, uint16_t,
                                                   int) override {
    return nullptr;
  }

  webrtc::AsyncPacketSocket* CreateClientTcpSocket(const webrtc::SocketAddress&,
                                                   const webrtc::SocketAddress&,
                                                   const webrtc::PacketSocketTcpOptions&) override {
    return nullptr;
  }

  std::unique_ptr<webrtc::AsyncDnsResolverInterface> CreateAsyncDnsResolver() override {
    return factory().CreateAsyncDnsResolver();
  }

 private:
  webrtc::BasicPacketSocketFactory& factory() {
    if (!factory_) {
      factory_ = std::make_unique<webrtc::BasicPacketSocketFactory>(
          webrtc::Thread::Current()->socketserver());
    }
    return *factory_;
  }

  std::shared_ptr<LossState> state_;
  std::unique_ptr<webrtc::BasicPacketSocketFactory> factory_;
};

// Deterministic protection is confined to the fixture. Production continues to
// use WebRTC's loss-adaptive FEC controller. RTP/FEC generation and recovery are
// the real WebRTC implementations over ICE/DTLS/SRTP.
class FixedProtection final : public webrtc::FecController {
 public:
  explicit FixedProtection(bool multi_packet = false) : multi_packet_(multi_packet) {}

  void SetProtectionCallback(webrtc::VCMProtectionCallback* callback) override {
    callback_.store(callback);
  }

  void SetProtectionMethod(bool fec, bool) override {
    enabled_ = fec;
  }

  void SetEncodingData(size_t, size_t, size_t, size_t) override {}

  uint32_t UpdateFecRates(uint32_t bitrate, int, uint8_t, std::vector<bool>, int64_t) override {
    webrtc::FecProtectionParams params;
    params.fec_rate = enabled_ ? (multi_packet_ ? 85 : 255) : 0;
    params.max_fec_frames = multi_packet_ ? 3 : 1;
    uint32_t video = 0, nack = 0, fec = 0;
    callback_.load()->ProtectionRequest(&params, &params, &video, &nack, &fec);
    return bitrate / 2;
  }

  void UpdateWithEncodedData(size_t, webrtc::VideoFrameType) override {}

  bool UseLossVectorMask() override {
    return false;
  }

 private:
  const bool multi_packet_;
  std::atomic<webrtc::VCMProtectionCallback*> callback_{nullptr};
  std::atomic<bool> enabled_{false};
};

class FixedProtectionFactory final : public webrtc::FecControllerFactoryInterface {
 public:
  explicit FixedProtectionFactory(bool multi_packet = false) : multi_packet_(multi_packet) {}

  std::unique_ptr<webrtc::FecController> CreateFecController(const webrtc::Environment&) override {
    return std::make_unique<FixedProtection>(multi_packet_);
  }

 private:
  const bool multi_packet_;
};

std::string local(native::NativePeer& peer, native::TransportRuntime& runtime, bool offer) {
  complete(peer.create_local(offer));
  const auto deadline = SteadyClock::now() + std::chrono::seconds(5);
  while (!peer.gathered && SteadyClock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  check(peer.gathered, "ICE gathering failed");
  return runtime.signaling->BlockingCall([&] {
    std::string sdp;
    peer.pc->local_description()->ToString(&sdp);
    return sdp;
  });
}

std::vector<uint8_t> canonical(std::span<const uint8_t> input) {
  std::vector<uint8_t> output;
  for (size_t i = 0; i < input.size(); ++i) {
    if (i + 4 <= input.size() && input[i] == 0 && input[i + 1] == 0 && input[i + 2] == 0 &&
        input[i + 3] == 1) {
      continue;  // Normalize four-byte Annex-B delimiters to three bytes.
    }
    output.push_back(input[i]);
  }
  return output;
}

void video_case(Codec codec, bool receiver_enabled, bool discard_fec) {
  const std::string name = codec == Codec::h265 ? "H265" : codec == Codec::h264 ? "H264" : "VP8";
  auto state = std::make_shared<LossState>();
  state->discard_fec = discard_fec;
  webrtc::PeerConnectionFactoryDependencies send_deps, receive_deps;
  send_deps.fec_controller_factory = std::make_unique<FixedProtectionFactory>();
  receive_deps.packet_socket_factory = std::make_unique<LossSocketFactory>(state);
  native::TransportRuntime sending(true, std::move(send_deps));
  native::TransportRuntime receiving(receiver_enabled, std::move(receive_deps));
  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.tcp_candidate_policy = webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled;
  QueueLimits limits;
  limits.max_age = std::chrono::seconds(5);
  native::NativePeer publisher(sending, config, limits), subscriber(receiving, config, limits);
  TrackDescriptor descriptor;
  descriptor.id = "video";
  descriptor.format.codec = codec;
  descriptor.format.width = 64;
  descriptor.format.height = 48;
  auto input = sending.signaling->BlockingCall([&] {
    return webrtc::make_ref_counted<native::EncodedVideoSender>(
        native::BindingToken{"video", 7, 1, true}, descriptor);
  });
  auto track = sending.factory->CreateVideoTrack(input, "video");
  webrtc::RtpTransceiverInit init;
  init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
  auto added = publisher.pc->AddTransceiver(track, init);
  check(added.ok(), "Cannot add video transceiver");
  const auto capabilities = sending.factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
  auto selected = native::select_transport_codecs(capabilities.codecs, name, {}, true);
  check(added.value()->SetCodecPreferences(selected).ok(), "Cannot select media/FlexFEC codecs");
  const auto offer = local(publisher, sending, true);
  check(offer.find("flexfec-03/90000") != std::string::npos &&
            offer.find("a=ssrc-group:FEC-FR ") != std::string::npos,
        "Offer lacks FlexFEC payload type or protected SSRC association");
  // Observe the offered repair PT even if the answer rejects it, so fallback
  // cannot pass merely because the fixture forgot which PT to count.
  const auto fec_line = offer.rfind("a=rtpmap:", offer.find(" flexfec-03/90000"));
  {
    std::lock_guard lock(state->mutex);
    state->fec_pt = std::stoi(offer.substr(fec_line + 9));
  }
  complete(subscriber.set_remote({webrtc::SdpType::kOffer, offer}));
  const auto answer = local(subscriber, receiving, false);
  check((answer.find("flexfec-03/90000") != std::string::npos) == receiver_enabled,
        "Answer did not respect remote FlexFEC support");
  complete(publisher.set_remote({webrtc::SdpType::kAnswer, answer}));
  subscriber.refresh_receivers();
  const auto deadline = SteadyClock::now() + std::chrono::seconds(5);
  while ((!publisher.connected || !subscriber.connected) && SteadyClock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  check(publisher.connected && subscriber.connected, "ICE/DTLS did not connect");
  const auto tracks = subscriber.remote_tracks();
  check(tracks.size() == 1, "Missing video receiver");
  const auto output = tracks.begin()->second.output;
  {
    std::lock_guard lock(state->mutex);
    for (const auto& c : added.value()->sender()->GetParameters().codecs) {
      if (c.name == name) {
        state->media_pt = c.payload_type;
      } else if (c.name == "flexfec-03") {
        state->fec_pt = c.payload_type;
      }
      check(c.name != "rtx", "RTX would invalidate the recovery test");
    }
    check(state->media_pt >= 0, "Missing negotiated media payload type");
  }
  const auto expected = fixture_video(codec, false);
  size_t received = 0;
  for (int i = 0; i < 100; ++i) {
    EncodedFrame frame;
    frame.buffer = EncodedBuffer(expected);
    frame.epoch = 7;
    frame.timestamp = 90000 + i * 3000;
    frame.duration = std::chrono::milliseconds(33);
    frame.keyframe = true;
    check(input->send(frame) != native::SendResult::dropped, "Valid input frame rejected");
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    for (const auto& unit : output->take_frames()) {
      const auto payload = unit.buffer.bytes();
      check(codec == Codec::vp8 ? std::vector<uint8_t>(payload.begin(), payload.end()) == expected
                                : canonical(payload) == canonical(expected),
            "Recovered media differs from the original access unit");
      ++received;
      std::lock_guard lock(state->mutex);
      state->delivered_timestamps.insert(uint32_t(unit.timestamp));
    }
    std::lock_guard lock(state->mutex);
    state->armed = i >= 10;
  }
  input->close();
  std::lock_guard lock(state->mutex);
  std::cout << name << " receiver_flexfec=" << receiver_enabled << " discard_fec=" << discard_fec
            << " media=" << state->media_packets << " fec=" << state->fec_packets
            << " dropped=" << state->dropped.size() << " recovered=" << state->recovered()
            << " received=" << received << '\n';
  check(received > 20 && !state->dropped.empty(), "Loss fixture did not exercise media");
  if (receiver_enabled) {
    check(state->fec_packets > 0, "Negotiated FlexFEC never emitted repair packets");
    if (!discard_fec) {
      check(state->recovered() >= 3, "Lost SRTP media was not recovered by FlexFEC");
    } else {
      check(state->recovered() == 0, "Recovery succeeded with all repair packets discarded");
    }
  } else {
    check(state->fec_packets == 0 && state->recovered() == 0,
          "Legacy peer unexpectedly used FlexFEC");
  }
}

std::string hex_text(std::string_view text) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const unsigned char c : text) {
    result += digits[c >> 4];
    result += digits[c & 15];
  }
  return result;
}

std::string unhex_text(std::string_view text) {
  check(text.size() % 2 == 0, "Invalid test IPC hex");
  std::string result;
  for (size_t i = 0; i < text.size(); i += 2) {
    result += static_cast<char>(std::stoul(std::string(text.substr(i, 2)), nullptr, 16));
  }
  return result;
}

// The Python harness handles LiveKit protobuf signaling. Media uses the SDK's
// actual native sender/receiver and connects directly to the Go SFU over SRTP.
// Only this fixture forces protection rates and drops outgoing ciphertext.
void sfu_case(Codec codec, bool discard_fec, bool downstream, bool receiver_fec) {
  const std::string name = codec == Codec::h265 ? "H265" : codec == Codec::h264 ? "H264" : "VP8";
  auto state = std::make_shared<LossState>();
  state->outgoing = !downstream;
  state->discard_fec = discard_fec;
  webrtc::PeerConnectionFactoryDependencies deps, receive_deps;
  deps.fec_controller_factory = std::make_unique<FixedProtectionFactory>(true);
  if (downstream) {
    receive_deps.packet_socket_factory = std::make_unique<LossSocketFactory>(state);
  } else {
    deps.packet_socket_factory = std::make_unique<LossSocketFactory>(state);
  }
  native::TransportRuntime sending(true, std::move(deps)),
      receiving(downstream && receiver_fec, std::move(receive_deps));
  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.tcp_candidate_policy = webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled;
  QueueLimits limits;
  limits.max_age = std::chrono::seconds(5);
  native::NativePeer publisher(sending, config, limits),
      publisher_control(receiving, config, limits), subscriber(receiving, config, limits);
  native::NativePeer* peers[] = {&publisher, &publisher_control, &subscriber};
  TrackDescriptor descriptor;
  descriptor.id = "video";
  descriptor.format.codec = codec;
  descriptor.format.width = 64;
  descriptor.format.height = 48;
  auto input = sending.signaling->BlockingCall([&] {
    return webrtc::make_ref_counted<native::EncodedVideoSender>(
        native::BindingToken{"video", 7, 1, true}, descriptor);
  });
  auto track = sending.factory->CreateVideoTrack(input, "video");
  webrtc::RtpTransceiverInit init;
  init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
  auto added = publisher.pc->AddTransceiver(track, init);
  check(added.ok(), "Cannot add SFU test video");
  const auto caps = sending.factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
  auto selected = native::select_transport_codecs(caps.codecs, name, {}, true);
  check(added.value()->SetCodecPreferences(selected).ok(), "Cannot select SFU test codec");
  const auto expected = fixture_video(codec, false);
  size_t received = 0;
  std::cout << "ready\n" << std::flush;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream command(line);
    std::string op;
    command >> op;
    if (op == "offer") {
      std::cout << "offer 0 " << hex_text(local(publisher, sending, true)) << '\n' << std::flush;
    } else if (op == "remote") {
      int id;
      std::string type, hex;
      command >> id >> type >> hex;
      check(id >= 0 && id < 3, "Invalid test peer id");
      const auto sdp = unhex_text(hex);
      if (id == 0) {
        check(sdp.find("flexfec-03/90000") != std::string::npos, "Missing upstream FlexFEC");
      }
      complete(peers[id]->set_remote(
          {type == "offer" ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer, sdp}));
      if (type == "offer") {
        if (id == 2) {
          const auto receive_caps =
              receiving.factory->GetRtpReceiverCapabilities(webrtc::MediaType::VIDEO);
          auto receive_codecs = native::select_transport_codecs(receive_caps.codecs, name, {},
                                                                downstream && receiver_fec);
          for (const auto& transceiver : subscriber.pc->GetTransceivers()) {
            if (transceiver->media_type() == webrtc::MediaType::VIDEO) {
              check(transceiver->SetCodecPreferences(receive_codecs).ok(),
                    "Cannot exclude downstream RTX");
            }
          }
          if (downstream && sdp.find("m=video ") != std::string::npos) {
            std::lock_guard lock(state->mutex);
            const auto media_pos = sdp.find(" " + name + "/90000");
            check(media_pos != std::string::npos, "Missing downlink media codec");
            state->media_pt = std::stoi(sdp.substr(sdp.rfind("a=rtpmap:", media_pos) + 9));
            const auto fec_pos = sdp.find(" flexfec-03/90000");
            check(fec_pos != std::string::npos &&
                      sdp.find("a=ssrc-group:FEC-FR ") != std::string::npos,
                  "Missing downlink FEC codec/SSRC group");
            state->fec_pt = std::stoi(sdp.substr(sdp.rfind("a=rtpmap:", fec_pos) + 9));
          }
        }
        const auto answer = local(*peers[id], receiving, false);
        if (id == 2 && sdp.find("m=video ") != std::string::npos) {
          check((answer.find("flexfec-03/90000") != std::string::npos) ==
                    (downstream && receiver_fec),
                "Wrong downstream FEC answer");
          check(answer.find(" rtx/90000") == std::string::npos, "Downstream RTX would hide loss");
          if (downstream) {
            // H.264 can negotiate several profiles/PTs. The SFU selects the
            // publisher-compatible profile, which need not be the first PT.
            std::lock_guard lock(state->mutex);
            state->media_pts.clear();
            std::istringstream lines(answer);
            std::string codec_line;
            while (std::getline(lines, codec_line)) {
              if (codec_line.starts_with("a=rtpmap:") &&
                  codec_line.find(" " + name + "/90000") != std::string::npos) {
                state->media_pts.insert(std::stoi(codec_line.substr(9)));
              }
            }
            check(!state->media_pts.empty(), "No negotiated downstream media payload types");
          }
        }
        std::cout << "answer " << id << ' ' << hex_text(answer) << '\n' << std::flush;
        peers[id]->refresh_receivers();
      } else {
        std::lock_guard lock(state->mutex);
        for (const auto& c : added.value()->sender()->GetParameters().codecs) {
          if (!downstream && c.name == name) {
            state->media_pt = c.payload_type;
          } else if (!downstream && c.name == "flexfec-03") {
            state->fec_pt = c.payload_type;
          }
          check(c.name != "rtx", "RTX would invalidate the SFU recovery test");
        }
        check(downstream || (state->media_pt >= 0 && state->fec_pt >= 0),
              "SFU did not accept media and FlexFEC");
        std::cout << "negotiated\n" << std::flush;
      }
    } else if (op == "ice") {
      int id, index;
      std::string mid, hex;
      command >> id >> index >> mid >> hex;
      check(id >= 0 && id < 3, "Invalid ICE test peer id");
      peers[id]->add_ice({mid, index, unhex_text(hex)});
    } else if (op == "tick") {
      int i;
      command >> i;
      {
        std::lock_guard lock(state->mutex);
        state->armed = i >= 60;
      }
      EncodedFrame frame;
      frame.buffer = EncodedBuffer(expected);
      frame.epoch = 7;
      frame.timestamp = 90000 + i * 3000;
      frame.duration = std::chrono::milliseconds(33);
      frame.keyframe = true;
      check(input->send(frame) != native::SendResult::dropped, "SFU test frame rejected");
      std::this_thread::sleep_for(std::chrono::milliseconds(35));
      for (const auto& [sid, remote] : subscriber.remote_tracks()) {
        for (const auto& unit : remote.output->take_frames()) {
          const auto payload = unit.buffer.bytes();
          check(codec == Codec::vp8
                    ? std::vector<uint8_t>(payload.begin(), payload.end()) == expected
                    : canonical(payload) == canonical(expected),
                "SFU delivered corrupt media");
          ++received;
          std::lock_guard lock(state->mutex);
          state->delivered_timestamps.insert(uint32_t(unit.timestamp));
        }
      }
      std::cout << "tick_done\n" << std::flush;
    } else if (op == "finish") {
      input->close();
      std::lock_guard lock(state->mutex);
      std::cout << "result {\"codec\":\"" << name << "\",\"discardFec\":" << discard_fec
                << ",\"downstream\":" << downstream << ",\"receiverFec\":" << receiver_fec
                << ",\"media\":" << state->media_packets << ",\"fec\":" << state->fec_packets
                << ",\"dropped\":" << state->dropped.size()
                << ",\"recovered\":" << state->recovered()
                << ",\"duplicateDrops\":" << state->duplicate_drops << ",\"received\":" << received
                << "}\n"
                << std::flush;
      check(publisher.connected && subscriber.connected && received > 30 &&
                state->dropped.size() >= 5,
            "SFU fixture did not exercise connected media and loss");
      check((downstream && !receiver_fec) ? state->fec_packets == 0 : state->fec_packets > 0,
            "Unexpected repair packet count");
      check((discard_fec || (downstream && !receiver_fec))
                ? state->recovered() == 0
                : state->recovered() == state->dropped.size(),
            "SFU FlexFEC recovery assertion failed");
      return;
    } else {
      throw std::runtime_error("Unknown SFU test command");
    }
  }
  throw std::runtime_error("SFU fixture ended before finish");
}

void options_and_selection() {
  LiveKitRoomOptions options;
  check(options.enable_flexfec, "FlexFEC should be enabled by default");
  options.enable_flexfec = false;
  const RoomOptions room = options;
  check(!std::dynamic_pointer_cast<const LiveKitConnectionOptions>(room.backend)->enable_flexfec,
        "Connection options lost the FlexFEC switch");
  native::TransportRuntime disabled(false), enabled(true);
  const auto off = disabled.factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO).codecs;
  const auto on = enabled.factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO).codecs;
  const auto has_fec = [](const auto& codecs) {
    return std::any_of(codecs.begin(), codecs.end(),
                       [](const auto& c) { return c.name == "flexfec-03"; });
  };
  check(!has_fec(off) && has_fec(on), "Per-runtime FlexFEC trials were not isolated");
  check(native::select_transport_codecs(on, "missing", {}, true).empty(),
        "Repair codec hid an unsupported media codec");
  check(native::select_transport_codecs(on, "H265", {{"profile-id", "999"}}, true).empty(),
        "Repair codec hid an unsupported fmtp");
  check(!has_fec(native::select_transport_codecs(on, "H265", {}, false)),
        "Disabled selection retained FlexFEC");
  native::NativePeer peer(disabled, webrtc::PeerConnectionInterface::RTCConfiguration{});
  check(peer.pc->AddTransceiver(webrtc::MediaType::VIDEO).ok(), "Cannot add disabled video track");
  check(complete(peer.create_local(true)).sdp.find("flexfec-03") == std::string::npos,
        "Disabled offer still advertised FlexFEC");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    webrtc::LogMessage::LogToDebug(webrtc::LS_ERROR);
    if (argc >= 3 && std::string(argv[1]) == "--sfu") {
      const std::string codec(argv[2]);
      check(codec == "vp8" || codec == "h264" || codec == "h265", "Invalid SFU test codec");
      const std::vector<std::string> flags(argv + 3, argv + argc);
      const auto has_flag = [&](const std::string& flag) {
        return std::find(flags.begin(), flags.end(), flag) != flags.end();
      };
      sfu_case(codec == "h265"   ? Codec::h265
               : codec == "h264" ? Codec::h264
                                 : Codec::vp8,
               has_flag("--discard-fec"), has_flag("--downstream"), !has_flag("--receiver-no-fec"));
      return 0;
    }
    options_and_selection();
    for (const auto codec : {Codec::vp8, Codec::h264, Codec::h265}) {
      video_case(codec, true, false);
    }
    video_case(Codec::h265, true, true);
    video_case(Codec::h265, false, false);
    std::cout << "FlexFEC negotiation, SRTP loss recovery and fallback passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
