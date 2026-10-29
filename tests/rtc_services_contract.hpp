#pragma once
#include "backends/rtc/stream_session.hpp"
#include "backends/rtc/data_track_session.hpp"
#include "backends/rtc/encryption.hpp"
#include <algorithm>
#include <thread>

namespace rtc_services_contract {
using namespace livekit::sdk;
using namespace livekit::sdk::detail;
using namespace std::chrono_literals;

static void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

template <class F>
static void rejects(F fn) {
  bool caught = false;
  try {
    fn();
  } catch (const std::exception&) {
    caught = true;
  }
  require(caught, "Expected rejection");
}

static std::span<const uint8_t> bytes(const std::string& s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

static void streams() {
  auto a = std::make_shared<StreamSession>(), b = std::make_shared<StreamSession>();
  a->connect();
  b->connect();
  std::promise<std::shared_ptr<StreamReader>> arrival;
  b->register_handler(true, "text", [&](auto reader) { arrival.set_value(reader); });
  StreamOptions options;
  options.topic = "text";
  options.total_size = 7;
  auto writer = a->open(true, options);
  auto first = writer->write(std::string("abc\xf0\x9f", 5));
  auto second = writer->write(std::string("\x8c\x8d", 2));
  auto end = writer->close();
  while (a->has_outgoing())
    a->pump([&](auto p) {
      p.set_participant_identity("alice");
      b->receive(p);
      return true;
    });
  first.get();
  second.get();
  end.get();
  auto received = arrival.get_future().get();
  require(received->read_all_text() == "abc\xf0\x9f\x8c\x8d", "UTF8 split across chunks");
  auto invalid = a->open(true, {});
  rejects([&] { invalid->write(std::string("\xc0\x80", 2)).get(); });
  auto incomplete = invalid->write(std::string("\xf0", 1));
  rejects([&] { invalid->close().get(); });
  auto abnormal = invalid->close("interrupted");
  while (a->has_outgoing())
    a->pump([](auto&) { return true; });
  incomplete.get();
  abnormal.get();
  auto stale = a->open(false, {});
  auto pending = stale->write("pending");
  a->disconnect();
  rejects([&] { pending.get(); });
  a->connect();
  rejects([&] { stale->write("old").get(); });
  std::stop_source stop;
  stop.request_stop();
  OperationOptions cancel;
  cancel.stop = stop.get_token();
  auto cancelled = a->open(false, {});
  rejects([&] { cancelled->write("cancel", cancel).get(); });
  StreamOptions slow_options;
  slow_options.stream_id = "slow";
  auto slow = a->open(false, slow_options);
  StreamOptions fast_options;
  fast_options.stream_id = "fast";
  auto fast = a->open(false, fast_options);
  auto bulk = slow->write(std::string(120000, 'x'));
  auto short_write = fast->write("y");
  std::vector<std::string> order;
  a->pump(
      [&](const auto& p) {
        order.push_back(p.has_stream_header() ? p.stream_header().stream_id()
                                              : p.stream_chunk().stream_id());
        return true;
      },
      6);
  require(short_write.wait_for(0ms) == std::future_status::ready &&
              bulk.wait_for(0ms) != std::future_status::ready,
          "Fair stream scheduler");
  a->disconnect();
  rejects([&] { bulk.get(); });
  std::promise<std::shared_ptr<StreamReader>> compressed;
  b->register_handler(false, "compressed", [&](auto reader) { compressed.set_value(reader); });
  livekit::DataPacket packet;
  packet.set_participant_identity("alice");
  auto* h = packet.mutable_stream_header();
  h->set_stream_id("compressed");
  h->set_topic("compressed");
  h->mutable_byte_header();
  h->set_compression(livekit::DataStream::DEFLATE_RAW);
  h->set_inline_content(deflate_raw(std::string(10000, 'z')));
  h->set_total_length(10000);
  b->receive(packet);
  require(compressed.get_future().get()->read_all() == std::vector<uint8_t>(10000, 'z'),
          "Compressed inline stream");
  require(inflate_raw(deflate_raw("hello"), 5) == "hello", "Raw DEFLATE");
  rejects([&] { inflate_raw(deflate_raw(std::string(10000, 'z')), 100); });
  b->disconnect();
}

static std::shared_ptr<ExternalKeyProvider> keys() {
  auto k = std::make_shared<ExternalKeyProvider>();
  EncryptionKey key{};
  k->set_key(0, key);
  return k;
}

static void encryption() {
  auto k = keys();
  const std::vector<uint8_t> mixed = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 1, 0x65, 0x88, 1, 2};
  const std::vector<uint8_t> canonical = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x65, 0x88, 1, 2};
  require(canonical_media_frame(mixed, EncryptionCodec::h264) == canonical,
          "Authenticated H264 headers survive RTP Annex-B reconstruction");
  // NIST SP800-38D AES-128-GCM zero key, zero IV, one zero block.
  const std::vector<uint8_t> cipher = {0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92,
                                       0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78,
                                       0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd,
                                       0xf5, 0x3a, 0x67, 0xb2, 0x12, 0x57, 0xbd, 0xdf};
  std::array<uint8_t, 12> iv{};
  require(decrypt_payload(*k, 0, iv, cipher) == std::vector<uint8_t>(16, 0),
          "Independent AES-GCM vector");
  for (auto codec : {EncryptionCodec::opus, EncryptionCodec::h264, EncryptionCodec::h265}) {
    FrameCryptor cryptor(k, codec);
    std::vector<uint8_t> plain = codec == EncryptionCodec::opus
                                     ? std::vector<uint8_t>{0xf8, 1, 2, 3, 0, 0, 1}
                                 : codec == EncryptionCodec::h264
                                     ? std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0x88, 1, 2, 0, 0, 1}
                                     : std::vector<uint8_t>{0, 0, 0, 1, 0x26, 1, 1, 2, 0, 0, 1};
    auto sealed = cryptor.encrypt(plain);
    require(cryptor.decrypt(sealed) == plain, "Media crypto roundtrip");
    auto again = cryptor.encrypt(plain);
    require(again != sealed, "Fresh nonce per frame");
    sealed[sealed.size() / 2] ^= 0x20;
    rejects([&] { cryptor.decrypt(sealed); });
  }
  livekit::DataPacket packet;
  packet.set_sequence(42);
  packet.set_participant_identity("alice");
  packet.mutable_user()->set_payload("secret");
  EncryptionOptions options;
  options.keys = k;
  auto sealed = encrypt_packet(packet, options);
  require(sealed.has_encrypted_packet() &&
              decrypt_packet(sealed, options).SerializeAsString() == packet.SerializeAsString(),
          "Encrypted packet routing metadata");
  EncryptionKey rotated{};
  rotated.fill(4);
  k->set_key(1, rotated);
  require(decrypt_packet(sealed, options).user().payload() == "secret",
          "Previous key remains available");
  require(encrypt_packet(packet, options).encrypted_packet().key_index() == 1, "Key rotation");
  k->remove_key(0);
  rejects([&] { decrypt_packet(sealed, options); });
  rejects([&] { decrypt_packet(packet, options); });
}

static void data_tracks(bool encrypted) {
  auto a = std::make_shared<DataTrackSession>(), b = std::make_shared<DataTrackSession>();
  if (encrypted) {
    EncryptionOptions o;
    o.keys = keys();
    a->configure(o);
    b->configure(o);
  }
  a->connect();
  b->connect();
  auto publication = a->publish("telemetry", {}, {});
  auto requests = a->take_signals();
  require(requests.size() == 1, "Publish signal");
  livekit::SignalResponse ack;
  auto* info = ack.mutable_publish_data_track_response()->mutable_info();
  info->set_pub_handle(requests.front().publish_data_track_request().pub_handle());
  info->set_name("telemetry");
  info->set_sid("DT_one");
  if (encrypted)
    info->set_encryption(livekit::Encryption::GCM);
  a->receive_signal(ack);
  auto local = publication.get();
  livekit::ParticipantInfo participant;
  participant.set_identity("alice");
  *participant.add_data_tracks() = *info;
  b->participant(participant);
  auto remote = b->remote_tracks().at(0);
  auto subscription = remote.subscribe();
  b->take_signals();
  livekit::SignalResponse handles;
  auto& entry = (*handles.mutable_data_track_subscriber_handles()->mutable_sub_handles())[7];
  entry.set_track_sid("DT_one");
  entry.set_publisher_identity("alice");
  b->receive_signal(handles);
  auto reader = subscription.get();
  DataFrame original;
  original.payload.resize(40000);
  for (size_t n = 0; n < original.payload.size(); ++n)
    original.payload[n] = uint8_t(n);
  original.user_timestamp = 0x123456789abcdef0;
  auto deliver = [&](const LocalDataTrack& track) {
    track.write(original);
    std::vector<std::vector<uint8_t>> packets;
    while (auto p = a->take_packet()) {
      a->packet_sent(*p, true);
      (*p)[2] = 0;
      (*p)[3] = 7;
      packets.push_back(std::move(*p));
    }
    std::reverse(packets.begin(), packets.end());
    for (auto& p : packets) {
      b->receive_packet(p);
      b->receive_packet(p);
    }
  };
  deliver(local);
  auto frame = reader->read(1s);
  require(frame && frame->payload == original.payload &&
              frame->user_timestamp == original.user_timestamp,
          "Out of order DataTrack reassembly with duplicates");
  require(reader->stats().accepted == 1 && local.stats().delivered == 1,
          "Exactly one completed frame");
  a->rejoin();
  b->rejoin();
  a->connect();
  b->connect();
  rejects([&] { local.write(original); });
  requests = a->take_signals();
  require(requests.size() == 1 && requests[0].has_publish_data_track_request(),
          "Restore publication intent");
  info->set_sid("DT_two");
  a->receive_signal(ack);
  *participant.mutable_data_tracks(0) = *info;
  b->participant(participant);
  require(!b->take_signals().empty(), "Restore subscription intent");
  entry.set_track_sid("DT_two");
  b->receive_signal(handles);
  auto restored = a->local_tracks().at(0);
  deliver(restored);
  frame = reader->read(1s);
  require(frame && frame->payload == original.payload, "Existing reader follows rejoin");
  a->disconnect();
  b->disconnect();
  rejects([&] { reader->read(0ms); });
}
}  // namespace rtc_services_contract
