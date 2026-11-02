#include "service_test_result.hpp"
#include "rtc_room_test_bridge.hpp"
#include "rtc_blocking_bridge.hpp"
#include "rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
#include <livekit/rtc/livekit.hpp>
#include <livekit/json.hpp>

#include "rtc_video_fixtures.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

namespace {
void check(bool ok, const char* message) {
  if (!ok) {
    std::cerr << message << '\n';
    std::abort();
  }
}

template <class T>
auto await(T result) {
  return require_result(std::move(result));
}

template <class T>
auto await(std::future<T> future) {
  check(future.wait_for(20s) == std::future_status::ready, "RTC operation timed out");
  return service_value(future.get());
}

std::vector<uint8_t> normalized(std::span<const uint8_t> bytes) {
  std::vector<uint8_t> result;
  for (size_t i = 0; i < bytes.size();) {
    if (i + 4 <= bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 &&
        bytes[i + 3] == 1) {
      result.insert(result.end(), {0, 0, 0, 1});
      i += 4;
    } else if (i + 3 <= bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
      result.insert(result.end(), {0, 0, 0, 1});
      i += 3;
    } else {
      result.push_back(bytes[i++]);
    }
  }
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "url output-directory mode(plain|encrypted|encoded|media-e2ee)\n";
    return 1;
  }
  const std::filesystem::path out = argv[2];
  const std::string mode = argv[3];
  check(mode == "plain" || mode == "encrypted" || mode == "encoded" || mode == "media-e2ee",
        "Unsupported integration mode");
  auto unhex = [](const std::string& s) {
    std::vector<uint8_t> result;
    check(s.size() % 2 == 0, "Odd hexadecimal fixture length");
    const auto nibble = [](char value) {
      if (value >= '0' && value <= '9') {
        return value - '0';
      }
      if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
      }
      if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
      }
      check(false, "Invalid hexadecimal fixture digit");
      return 0;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
      result.push_back(uint8_t(nibble(s[i]) * 16 + nibble(s[i + 1])));
    }
    return result;
  };
  auto hex = [](const std::vector<uint8_t>& data) {
    std::string result;
    for (auto v : data) {
      result += "0123456789abcdef"[v >> 4];
      result += "0123456789abcdef"[v & 15];
    }
    return result;
  };
  auto keys = std::make_shared<ExternalKeyProvider>();
  EncryptionKey zero{};
  service_value(keys->set_key(0, zero));
  Json vectors;
  {
    std::ifstream f(out / "go-vectors.json");
    check(bool(f), "Cannot open Go vectors");
    vectors = Json::parse(f, nullptr, false);
    check(vectors.is_array(), "Invalid Go vector JSON");
  }
  for (auto& vector : vectors) {
    check(vector.is_object(), "Invalid Go vector object");
    const auto codec = vector.find("codec"), plain_bytes = vector.find("plain"),
               sealed = vector.find("sealed");
    check(codec != vector.end() && codec->is_string() && plain_bytes != vector.end() &&
              plain_bytes->is_string() && sealed != vector.end() && sealed->is_string(),
          "Invalid Go vector fields");
    const auto& name = codec->get_ref<const std::string&>();
    FrameCryptor cryptor(keys, name == "opus"   ? EncryptionCodec::opus
                               : name == "h264" ? EncryptionCodec::h264
                                                : EncryptionCodec::h265);
    auto plain = unhex(plain_bytes->get_ref<const std::string&>());
    check(service_value(cryptor.decrypt(unhex(sealed->get_ref<const std::string&>()))) == plain,
          "Go media crypto incompatible");
    vector["sealed"] = hex(service_value(cryptor.encrypt(plain)));
  }
  {
    std::ofstream f(out / "cpp-vectors.json");
    f << vectors;
  }
  const bool media_mode = mode == "encoded" || mode == "media-e2ee";
  auto room = require_result(livekit::sdk::app_support::create_blocking_room());
  livekit::sdk::app_support::RtcEventMailbox room_events(*room);
  auto services = require_result(room->services());
  auto text = std::make_shared<std::promise<Result<std::string>>>();
  auto binary = std::make_shared<std::promise<Result<std::vector<uint8_t>>>>();
  auto text_result = text->get_future();
  auto binary_result = binary->get_future();
  service_value(services->register_stream(true, "reply-text", [text](auto reader) {
    text->set_value(reader->read_all_text());
    return Result<void>::success();
  }));
  service_value(services->register_stream(false, "reply-bytes", [binary](auto reader) {
    binary->set_value(reader->read_all());
    return Result<void>::success();
  }));
  LiveKitRoomOptions options;
  options.url = argv[1];
  options.signal_protocol = 17;
  options.reconnect.enabled = false;
  const auto token = std::getenv("LIVEKIT_TOKEN");
  check(token && *token, "Missing LIVEKIT_TOKEN");
  options.token = token;
  if (mode == "encrypted" || mode == "media-e2ee") {
    options.encryption.keys = keys;
    options.encryption.media = mode == "media-e2ee";
  }
  await(room->join(options));
  std::jthread media_pump;
  if (media_mode) {
    TrackDescriptor d;
    d.id = d.name = "cpp-encoded";
    d.format.codec = Codec::h264;
    d.format.width = 64;
    d.format.height = 48;
    auto track = await(room->publish(d));
    media_pump = std::jthread([track](std::stop_token stop) {
      uint64_t timestamp = 0;
      while (!stop.stop_requested()) {
        EncodedFrame frame;
        frame.buffer = EncodedBuffer(fixture_video(Codec::h264, false));
        frame.keyframe = true;
        frame.timestamp = timestamp;
        timestamp += 3000;
        frame.duration = 33333us;
        frame.epoch = track->snapshot().epoch;
        track->try_write(std::move(frame));
        std::this_thread::sleep_for(33ms);
      }
    });
  }
  StreamOptions stream;
  stream.topic = "request-text";
  stream.destination_identities = {"go-peer"};
  std::string expected;
  for (int i = 0; i < 10000; ++i) {
    expected += "中文🌍";
  }
  await(services->send_text(expected, stream));
  std::vector<uint8_t> payload(300000);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = uint8_t(i * 31);
  }
  const auto file = out / "input.bin";
  {
    std::ofstream f(file, std::ios::binary);
    f.write(reinterpret_cast<const char*>(payload.data()), payload.size());
  }
  stream.topic = "request-bytes";
  await(services->send_file(file, stream));
  check(await(std::move(text_result)) == expected && await(std::move(binary_result)) == payload,
        "Go stream echo content mismatch");
  auto tracks = services->remote_data_tracks();
  check(!tracks.empty(), "Go DataTrack missing from initial roster");
  auto data_reader = await(tracks.front().subscribe());
  auto local_data = await(services->publish_data_track("cpp-data"));
  DataFrame data;
  data.payload.resize(40000);
  data.user_timestamp = 12345;
  for (size_t i = 0; i < data.payload.size(); ++i) {
    data.payload[i] = uint8_t(i * 17);
  }
  bool echoed = false;
  for (int i = 0; i < 100 && !echoed; ++i) {
    service_value(local_data.write(data));
    auto frame = data_reader->read(100ms);
    if (!frame) {
      check(frame.error().code == ErrorCode::timeout, "DataTrack read failed");
    } else if (*frame) {
      check((**frame).payload == data.payload && (**frame).user_timestamp == data.user_timestamp,
            "Go DataTrack echo mismatch");
      echoed = true;
    }
  }
  check(echoed, "Go DataTrack echo timed out");
  if (media_mode) {
    std::shared_ptr<SubscribedTrack> receiver;
    const auto deadline = SteadyClock::now() + 20s;
    while (!receiver && SteadyClock::now() < deadline) {
      auto event = service_value(room_events.take_event(100ms));
      if (event && event->kind == RoomEventKind::remote_track_added &&
          event->descriptor->name == "go-encoded-echo") {
        receiver = await(room->subscribe(*event->descriptor));
      }
    }
    check(bool(receiver), "Go encoded echo was not discovered");
    auto frame = receiver->read(10s);
    check(frame.status == ReadStatus::frame && normalized(frame.frame->buffer.bytes()) ==
                                                   normalized(fixture_video(Codec::h264, false)),
          "Go encoded echo payload mismatch");
    check(std::filesystem::exists(out / "go-video"), "Go video reception marker missing");
  }
  std::ofstream(out / "cpp-result.json") << Json{
      {"ok", true},
      {"backend", "encoded"},
      {"mode", mode},
      {"textBytes", expected.size()},
      {"fileBytes", payload.size()},
      {"dataTrackBytes", data.payload.size()}}.dump(2);
  std::ofstream(out / "cpp-done") << "done";
  const auto done_deadline = SteadyClock::now() + 3s;
  while (!std::filesystem::exists(out / "go-result.json") && SteadyClock::now() < done_deadline) {
    std::this_thread::sleep_for(50ms);
  }
  if (media_pump.joinable()) {
    media_pump.request_stop();
    media_pump.join();
  }
  await(room->leave());
  return 0;
}
