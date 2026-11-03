#include "backends/livekit/data/native_room_data.hpp"
#include "backends/livekit/data/packet_encryption.hpp"
#include "backends/livekit/region_policy.hpp"
#include "rtc_video_fixtures.hpp"
#include <atomic>
#include "livekit/client/room.hpp"
#include <type_traits>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

using namespace livekit::sdk;
using namespace livekit::sdk::client;
namespace impl = livekit::sdk::client::detail;
using namespace std::chrono_literals;

static_assert(
    std::is_same_v<decltype(std::declval<LocalParticipant>().perform_rpc(RpcCallOptions{})),
                   Result<std::string>>);
static_assert(std::is_same_v<decltype(std::declval<LocalDataTrack>().write(DataFrame{})), void>);
static_assert(std::is_same_v<decltype(std::declval<StreamWriter>().close()), void>);
static_assert(
    std::is_same_v<std::invoke_result_t<StreamHandler, std::shared_ptr<StreamReader>>, void>);
static_assert(std::is_same_v<
              decltype(std::declval<ExternalKeyProvider>().set_key(0, EncryptionKey{})), void>);

namespace {
void check(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

template <class F>
void wait_for(F predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) {
    check(std::chrono::steady_clock::now() < deadline, "service deadline");
    std::this_thread::sleep_for(2ms);
  }
}

struct Pair {
  impl::NativeRoomData a, b;
  EncryptionOptions encryption;
  std::atomic<bool> stopping{false};
  std::thread pump;

  Pair() {
    auto keys = std::make_shared<ExternalKeyProvider>();
    keys->set_key(0, EncryptionKey{});
    encryption.keys = keys;
    check(bool(a.connect(encryption)) && bool(b.connect(encryption)), "service connect");
    pump = std::thread([this] {
      while (!stopping) {
        auto send = [&](impl::NativeRoomData& to, const char* identity,
                        livekit::DataPacket packet) {
          packet.set_participant_identity(identity);
          auto wire = impl::encrypt_packet(packet, encryption);
          check(bool(wire) && wire->has_encrypted_packet(), "services use encrypted wire packets");
          auto plain = impl::decrypt_packet(*wire, encryption);
          check(bool(plain), "service packet decrypt");
          to.receive(*plain);
          return Result<bool>::success(true);
        };
        auto signal = [](const livekit::SignalRequest&) { return Result<void>::success(); };
        auto track = [](const std::vector<uint8_t>&) { return true; };
        check(bool(a.pump([&](const auto& p) { return send(b, "a", p); }, signal, track)),
              "pump a");
        check(bool(b.pump([&](const auto& p) { return send(a, "b", p); }, signal, track)),
              "pump b");
        std::this_thread::sleep_for(1ms);
      }
    });
  }

  ~Pair() {
    stopping = true;
    pump.join();
    a.interrupt(false);
    b.interrupt(false);
  }
};

void rpc_and_streams() {
  Pair pair;
  check(bool(pair.b.register_rpc("echo", [](RpcInvocation v) { return RpcResult{v.payload, {}}; })),
        "register RPC");
  RpcCallOptions call;
  call.destination_identity = "b";
  call.method = "echo";
  call.payload = "hello 世界";
  auto result = pair.a.perform_rpc(call);
  check(result && *result == call.payload, "RPC response");
  call.compress = true;
  check(bool(pair.a.perform_rpc(call)), "compressed RPC");
  call.method = "missing";
  result = pair.a.perform_rpc(call);
  check(!result && result.error().backend_reason == 1400, "unsupported RPC method");
  call.method = "echo";
  call.payload.assign(16000, 'x');
  call.compress = false;
  result = pair.a.perform_rpc(call);
  check(!result && result.error().backend_reason == 1402, "RPC payload limit");

  std::promise<std::string> received;
  auto text = received.get_future();
  check(bool(pair.b.register_stream(true, "text",
                                    [&](std::shared_ptr<StreamReader> reader) {
                                      auto all = reader->read_all_text();
                                      check(bool(all), "text reader completed");
                                      received.set_value(std::move(*all));
                                    })),
        "register text stream");
  StreamOptions options;
  options.topic = "text";
  options.destination_identities = {"b"};
  std::string payload;
  for (int i = 0; i < 12000; ++i) {
    payload += "文";
  }
  auto sent = pair.a.send_text(payload, options, {});
  check(sent && sent->total_size == payload.size(), "text stream send");
  check(text.wait_for(3s) == std::future_status::ready && text.get() == payload,
        "chunked UTF8 text stream");

  std::promise<std::shared_ptr<StreamReader>> acquired;
  auto reader = acquired.get_future();
  check(
      bool(pair.b.register_stream(false, "bytes", [&](auto value) { acquired.set_value(value); })),
      "register byte stream");
  options.topic = "bytes";
  auto writer = pair.a.open_stream(false, options);
  check(bool(writer), "open stream");
  (*writer)->write(std::string("first"));
  check(bool((*writer)->status()), "synchronous stream write status");
  check(reader.wait_for(3s) == std::future_status::ready, "byte reader delivered");
  auto handle = reader.get();
  auto chunk = handle->read(1s);
  check(chunk && *chunk && std::string((**chunk).begin(), (**chunk).end()) == "first",
        "byte stream chunk");
  (*writer)->close();
  check(bool((*writer)->status()), "stream trailer submitted");
  auto eof = handle->read(1s);
  check(eof && !*eof, "stream EOF");
}

void crypto() {
  auto keys = std::make_shared<ExternalKeyProvider>();
  keys->set_key(0, EncryptionKey{});
  // NIST AES-128-GCM zero-key/zero-IV known-answer vector, independent of encrypt().
  const std::vector<uint8_t> sealed = {0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92,
                                       0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78,
                                       0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd,
                                       0xf5, 0x3a, 0x67, 0xb2, 0x12, 0x57, 0xbd, 0xdf};
  std::array<uint8_t, 12> iv{};
  auto plain = impl::decrypt_payload(*keys, 0, iv, sealed);
  check(plain && *plain == std::vector<uint8_t>(16, 0), "NIST GCM vector");
  for (auto codec : {EncryptionCodec::opus, EncryptionCodec::vp8, EncryptionCodec::h264,
                     EncryptionCodec::h265}) {
    auto bytes = codec == EncryptionCodec::opus
                     ? std::vector<uint8_t>{0xf8, 0xff, 0xfe}
                     : fixture_video(codec == EncryptionCodec::vp8    ? rtc::Codec::vp8
                                     : codec == EncryptionCodec::h264 ? rtc::Codec::h264
                                                                      : rtc::Codec::h265,
                                     false);
    bytes = impl::canonical_media_frame(bytes, codec);
    FrameCryptor cryptor(keys, codec);
    auto cipher = cryptor.encrypt(bytes);
    check(cipher && *cipher != bytes, "media ciphertext differs");
    auto decoded = cryptor.decrypt(*cipher);
    check(decoded && *decoded && **decoded == bytes, "media cryptor roundtrip");
    cipher->back() = 15;
    check(!cryptor.decrypt(*cipher), "unknown key rejected");
  }
  livekit::DataPacket packet;
  packet.mutable_user()->set_payload("secret");
  EncryptionOptions options;
  options.keys = keys;
  auto encrypted = impl::encrypt_packet(packet, options);
  check(encrypted && encrypted->has_encrypted_packet(), "packet encryption");
  check(!impl::decrypt_packet(packet, options), "plaintext downgrade rejected");
  auto tampered = *encrypted;
  auto bytes = tampered.encrypted_packet().encrypted_value();
  bytes[0] ^= 1;
  tampered.mutable_encrypted_packet()->set_encrypted_value(bytes);
  check(!impl::decrypt_packet(tampered, options), "tampered ciphertext rejected");
  keys->set_key(1, EncryptionKey{1});
  check(bool(impl::decrypt_packet(*encrypted, options)), "previous rotation key retained");
  keys->remove_key(0);
  check(!impl::decrypt_packet(*encrypted, options), "removed key rejected");
}

void data_track_rejoin() {
  auto tracks = std::make_shared<impl::DataTrackSession>();
  tracks->connect();
  auto pending = tracks->publish("state", {}, {});
  auto signals = tracks->take_signals();
  check(signals && signals->size() == 1, "data publish signal");
  const auto handle = signals->front().publish_data_track_request().pub_handle();
  livekit::SignalResponse ack;
  auto* info = ack.mutable_publish_data_track_response()->mutable_info();
  info->set_sid("DT_before");
  info->set_name("state");
  info->set_pub_handle(handle);
  check(bool(tracks->receive_signal(ack)), "data publish response");
  auto published = pending.get();
  check(published && published->info(), "published data handle");
  tracks->rejoin();
  check(!published->info(), "old data handle cannot cross generation");
  check(tracks->local_tracks().empty(), "restoring track not yet advertised");
  tracks->connect();
  signals = tracks->take_signals();
  check(signals && signals->size() == 1 &&
            signals->front().publish_data_track_request().name() == "state",
        "rejoin republishes data track");
  info->set_sid("DT_after");
  check(bool(tracks->receive_signal(ack)), "republication response");
  auto restored = tracks->local_tracks();
  check(restored.size() == 1 && restored[0].info() && restored[0].info()->sid == "DT_after",
        "new data handle after rejoin");
  tracks->disconnect();
  check(!restored[0].info(), "closed data handle rejected");
}

void cancellation() {
  impl::NativeRoomData data;
  check(bool(data.connect({})), "connect pending service");
  RpcCallOptions short_call;
  short_call.method = "echo";
  short_call.destination_identity = "absent";
  short_call.response_timeout = 30ms;
  auto timeout = data.perform_rpc(short_call);
  check(!timeout && timeout.error().backend_reason == 1502, "RPC response timeout");
  livekit::compat::stop_source cancel;
  short_call.stop = cancel.get_token();
  cancel.request_stop();
  auto cancelled = data.perform_rpc(short_call);
  check(!cancelled && cancelled.error().code == ErrorCode::cancelled, "RPC stop token");

  RpcCallOptions call;
  call.method = "echo";
  call.destination_identity = "absent";
  auto pending = std::async(std::launch::async, [&] { return data.perform_rpc(call); });
  std::this_thread::sleep_for(20ms);
  data.interrupt(false);
  check(pending.wait_for(1s) == std::future_status::ready && !pending.get(),
        "close releases RPC wait");
  check(bool(data.connect({})), "reconnect services");
  auto publication =
      std::async(std::launch::async, [&] { return data.publish_data_track("pending", {}, {}); });
  std::this_thread::sleep_for(20ms);
  data.interrupt(false);
  check(publication.wait_for(1s) == std::future_status::ready && !publication.get(),
        "close releases data publish wait");
}

void shutdown_fences_handlers() {
  impl::NativeRoomData data;
  check(bool(data.connect({})), "connect handler lifetime test");
  std::atomic<bool> rpc_done{false}, stream_done{false};
  std::promise<void> rpc_entered, stream_entered;
  auto rpc_ready = rpc_entered.get_future();
  auto stream_ready = stream_entered.get_future();
  Room room;
  check(bool(data.register_rpc(
            "wait",
            [&](RpcInvocation call) {
              room.close();  // Reentrant close is rejected before any state changes.
              check(room.connection_state() == ConnectionState::created, "handler close rejected");
              rpc_entered.set_value();
              while (!call.stop.stop_requested()) {
                std::this_thread::sleep_for(1ms);
              }
              rpc_done = true;
              return RpcResult{};
            })),
        "register cancellation handler");
  check(bool(data.register_stream(true, "wait",
                                  [&](auto reader) {
                                    stream_entered.set_value();
                                    auto chunk = reader->read(10s);
                                    check(!chunk && chunk.error().code == ErrorCode::session_closed,
                                          "shutdown cancels active reader");
                                    stream_done = true;
                                  })),
        "register blocking stream handler");
  livekit::DataPacket request;
  request.set_participant_identity("peer");
  auto* rpc = request.mutable_rpc_request();
  rpc->set_id("shutdown-request");
  rpc->set_method("wait");
  rpc->set_version(1);
  rpc->set_response_timeout_ms(10000);
  data.receive(request);
  livekit::DataPacket stream;
  stream.set_participant_identity("peer");
  auto* header = stream.mutable_stream_header();
  header->set_stream_id("shutdown-stream");
  header->set_topic("wait");
  header->mutable_text_header();
  data.receive(stream);
  check(rpc_ready.wait_for(2s) == std::future_status::ready, "RPC handler entered");
  check(stream_ready.wait_for(2s) == std::future_status::ready, "stream handler entered");
  data.shutdown();
  check(rpc_done && stream_done, "shutdown fences both application handlers");
  data.shutdown();
}
}  // namespace

int main() {
  RegionOptions regions;
  check(!impl::trusted_destination("wss://test.livekit.cloud", "ws://test.livekit.cloud", regions),
        "TLS downgrade denied");
  check(!impl::trusted_destination("wss://example.org", "wss://other.org", regions),
        "untrusted redirect denied");
  regions.trusted_endpoints = {"wss://other.org"};
  check(bool(impl::trusted_destination("wss://example.org", "https://other.org", regions)),
        "explicit trusted endpoint");
  data_track_rejoin();
  crypto();
  rpc_and_streams();
  cancellation();
  shutdown_fences_handlers();
  std::cout << "PASS room service protocols, synchronous controls, cancellation, crypto and region "
               "policy\n";
}
