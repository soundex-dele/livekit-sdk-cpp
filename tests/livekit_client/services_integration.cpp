#include "livekit/client/room.hpp"
#include "livekit/auth.hpp"
#include "rtc_video_fixtures.hpp"
#include <atomic>
#include <cstdlib>
#include <future>
#include <fstream>
#include <iostream>
#include <thread>

using namespace livekit::sdk::client;
using namespace std::chrono_literals;
namespace sdk = livekit::sdk;

namespace {
void check(bool value, const char* message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

template <class F>
void until(F predicate, const char* message) {
  const auto end = SteadyClock::now() + 12s;
  while (!predicate()) {
    check(SteadyClock::now() < end, message);
    std::this_thread::sleep_for(10ms);
  }
}

struct Events : RoomObserver {
  std::atomic<int> data{0}, errors{0}, dtmf{0}, transcription{0}, decrypted{0};

  void on_room_event(const RoomEvent& event) override {
    if (event.kind == RoomEventKind::data_received) {
      ++data;
    }
    if (event.kind == RoomEventKind::data_error) {
      ++errors;
    }
    if (event.kind == RoomEventKind::dtmf_received) {
      ++dtmf;
    }
    if (event.kind == RoomEventKind::transcription_received) {
      ++transcription;
    }
    if (event.encryption && event.encryption->ok && !event.encryption->encrypting) {
      ++decrypted;
    }
  }
};

void run(const std::string& url, const char* key, const char* secret, bool encrypted,
         const std::filesystem::path& output) {
  auto token = [&](const char* identity) {
    auto result =
        sdk::make_token(key, secret, identity,
                        sdk::Json{{"roomJoin", true},
                                  {"room", encrypted ? "encrypted-services" : "plain-services"},
                                  {"canPublish", true},
                                  {"canSubscribe", true},
                                  {"canPublishData", true},
                                  {"agent", std::string(identity) == "publisher"}});
    check(bool(result), "token creation");
    return *result;
  };
  Events events;
  Room publisher, subscriber;
  subscriber.set_observer(&events);
  auto sender_keys = std::make_shared<ExternalKeyProvider>();
  auto receiver_keys = std::make_shared<ExternalKeyProvider>();
  sender_keys->set_key(0, EncryptionKey{1, 2, 3, 4});
  receiver_keys->set_key(0, EncryptionKey{1, 2, 3, 4});
  ConnectionOptions options;
  options.url = url;
  options.token = token("publisher");
  options.connect_timeout = 20;
  options.operation_timeout = 10s;
  options.reconnect.enabled = false;
  if (encrypted) {
    options.encryption.keys = sender_keys;
    options.encryption.media = true;
  }
  std::atomic<int> provider_calls{0};
  const auto valid_token = options.token;
  options.token.clear();
  options.token_provider = [&](const TokenRequest& request) {
    ++provider_calls;
    return sdk::Result<std::string>::success(
        request.reason == TokenRequestReason::initial ? "invalid-test-token" : valid_token);
  };
  check(publisher.connect(options), "synchronous token provider authentication retry");
  check(provider_calls == 2, "provider invoked for initial and authentication failure");
  options.token_provider = {};
  options.token = token("subscriber");
  if (encrypted) {
    options.encryption.keys = receiver_keys;
  }
  options.url = "http://127.0.0.1:1";
  options.regions.trusted_endpoints = {url};
  options.regions.fallback_endpoints = {url};
  options.regions.endpoint_timeout = 1500ms;
  check(subscriber.connect(options), "trusted endpoint failover");
  auto a = publisher.local_participant(), b = subscriber.local_participant();
  b.register_rpc("echo", [](RpcInvocation v) { return RpcResult{"reply:" + v.payload, {}}; });
  RpcCallOptions call;
  call.destination_identity = "subscriber";
  call.method = "echo";
  call.payload = "中文 RPC";
  call.response_timeout = 4s;
  auto rpc = a.perform_rpc(call);
  if (!rpc) {
    std::cerr << rpc.error().message << " reason=" << rpc.error().backend_reason << '\n';
  }
  check(rpc && *rpc == "reply:" + call.payload, "RPC over SFU");
  call.method = "missing";
  rpc = a.perform_rpc(call);
  check(!rpc && rpc.error().backend_reason == 1400, "remote RPC error");

  std::promise<std::string> received;
  auto text = received.get_future();
  b.register_stream(true, "text", [&](auto reader) {
    auto value = reader->read_all_text();
    check(bool(value), "text reader completed");
    received.set_value(std::move(*value));
  });
  StreamOptions stream;
  stream.topic = "text";
  stream.destination_identities = {"subscriber"};
  std::string body;
  for (int i = 0; i < 9000; ++i) {
    body += "字";
  }
  auto sent = a.send_text(body, stream);
  check(bool(sent), "text stream send");
  check(text.wait_for(8s) == std::future_status::ready && text.get() == body,
        "text stream reassembly");

  std::promise<std::vector<uint8_t>> file_received;
  auto file_data = file_received.get_future();
  b.register_stream(false, "file", [&](auto reader) {
    auto value = reader->read_all();
    check(bool(value), "file reader completed");
    file_received.set_value(std::move(*value));
  });
  const auto path = output / (encrypted ? "encrypted_文件.bin" : "plain_文件.bin");
  const std::vector<uint8_t> contents(40000, 0xa5);
  {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(contents.data()), contents.size());
    check(bool(file), "file fixture");
  }
  stream.topic = "file";
  auto file_sent = a.send_file(path, stream);
  check(bool(file_sent), "file stream send");
  check(file_data.wait_for(8s) == std::future_status::ready && file_data.get() == contents,
        "file stream and Unicode path");

  auto data_track = a.publish_data_track("telemetry");
  if (!data_track) {
    std::cerr << data_track.error().message << '\n';
  }
  check(bool(data_track), "data track publish ACK");
  std::optional<RemoteDataTrack> remote;
  until(
      [&] {
        for (auto participant : subscriber.remote_participants()) {
          for (auto track : participant.data_tracks()) {
            if (track.info().name == "telemetry") {
              remote = track;
            }
          }
        }
        return bool(remote);
      },
      "remote data track discovery");
  auto data_stream = remote->subscribe();
  check(bool(data_stream), "data track subscription ACK");
  DataFrame frame;
  frame.payload.assign(60000, 0x42);
  frame.user_timestamp = 12345;
  data_track->write(frame);
  auto data = (*data_stream)->read(5s);
  check(data && *data && (**data).payload == frame.payload &&
            (**data).user_timestamp == frame.user_timestamp,
        "fragmented data track frame");
  data_track->unpublish();
  auto ended = (*data_stream)->read(5s);
  check(!ended || !*ended, "data track unpublish closes reader");

  a.publish_dtmf({1, "1"});
  a.publish_transcription({"publisher", "transcript-track", {{"one", "hello", 1, 2, true, "en"}}});
  a.publish_data({{1, 2, 3}, true, "data", {"subscriber"}});
  until([&] { return events.data > 0 && events.dtmf > 0 && events.transcription > 0; },
        "data and typed Observer events");
  if (encrypted) {
    receiver_keys->remove_key(0);
    const int count = events.data.load();
    a.publish_data({{4, 5, 6}, true, "data", {"subscriber"}});
    until([&] { return events.errors > 0; }, "missing key reported");
    check(events.data == count, "missing key does not deliver plaintext");
    sender_keys->set_key(1, EncryptionKey{5, 6, 7, 8});
    receiver_keys->set_key(1, EncryptionKey{5, 6, 7, 8});
    a.publish_data({{7, 8, 9}, true, "data", {"subscriber"}});
    until([&] { return events.data > count; }, "key rotation");
  }

  CodecFormat format;
  format.codec = Codec::vp8;
  format.width = 64;
  format.height = 48;
  auto track = LocalTrack::create_video("video", format);
  check(bool(track) && bool(a.publish_track(*track)), "media publication");
  std::optional<RemoteTrack> video;
  until(
      [&] {
        for (auto participant : subscriber.remote_participants()) {
          for (auto publication : participant.track_publications()) {
            video = publication.track();
          }
        }
        return bool(video);
      },
      "media subscription");
  const auto pixels = fixture_video(sdk::rtc::Codec::vp8, false);
  bool matched = false;
  for (uint64_t i = 0; i < 80 && !matched; ++i) {
    EncodedFrame encoded;
    encoded.buffer = EncodedBuffer(pixels);
    encoded.timestamp = i * 3600;
    encoded.epoch = track->feedback().epoch;
    encoded.duration = 40ms;
    encoded.keyframe = true;
    check(track->try_write(std::move(encoded)) == WriteStatus::accepted,
          "encoded input submission");
    auto received_frame = video->read({100ms});
    if (received_frame.frame) {
      auto bytes = received_frame.frame->buffer.bytes();
      matched = std::vector<uint8_t>(bytes.begin(), bytes.end()) == pixels;
    }
  }
  check(matched, "media decrypted to original VP8 access unit");
  if (encrypted) {
    until([&] { return events.decrypted > 0; }, "media cryptor Observer state");
  }
  CodecFormat audio_format;
  audio_format.codec = Codec::opus;
  audio_format.clock_rate = 48000;
  audio_format.channels = 1;
  auto audio_track = LocalTrack::create_audio("audio", audio_format);
  check(audio_track && a.publish_track(*audio_track), "Opus publication");
  std::optional<RemoteTrack> audio;
  until(
      [&] {
        for (auto participant : subscriber.remote_participants()) {
          for (auto publication : participant.track_publications()) {
            if (publication.snapshot().kind == MediaKind::audio) {
              audio = publication.track();
            }
          }
        }
        return bool(audio);
      },
      "Opus subscription");
  matched = false;
  const std::vector<uint8_t> silence{0xf8, 0xff, 0xfe};
  for (uint64_t i = 0; i < 80 && !matched; ++i) {
    EncodedFrame encoded;
    encoded.buffer = EncodedBuffer(silence);
    encoded.timestamp = i * 960;
    encoded.epoch = audio_track->feedback().epoch;
    encoded.duration = 20ms;
    check(audio_track->try_write(std::move(encoded)) == WriteStatus::accepted,
          "Opus encoded submission");
    auto received_frame = audio->read({50ms});
    if (received_frame.frame) {
      auto bytes = received_frame.frame->buffer.bytes();
      matched = std::vector<uint8_t>(bytes.begin(), bytes.end()) == silence;
    }
  }
  check(matched, "media decrypted to original Opus access unit");
  publisher.close();
  subscriber.close();
  std::cout << "PASS " << (encrypted ? "encrypted" : "plain") << " room services and media\n";
}
}  // namespace

int main(int argc, char** argv) {
  const char *url = std::getenv("LIVEKIT_URL"), *key = std::getenv("LIVEKIT_API_KEY"),
             *secret = std::getenv("LIVEKIT_API_SECRET");
  if (!url || !key || !secret) {
    return 77;
  }
  const std::filesystem::path output =
      argc > 1 ? std::filesystem::path(argv[1]) : "room-services-output";
  std::error_code error;
  std::filesystem::create_directories(output, error);
  check(!error, "test output directory");
  run(url, key, secret, false, output);
  run(url, key, secret, true, output);
}
