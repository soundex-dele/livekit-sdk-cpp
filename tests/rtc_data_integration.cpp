#include "rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "data_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
#include "livekit/rtc/livekit.hpp"
#include "livekit/error.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

namespace {
void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

template <class T>
auto await(livekit::sdk::Result<T> result) {
  return require_result(std::move(result));
}

template <class T>
auto await(T future) {
  check(future.wait_for(15s) == std::future_status::ready, "Data operation timed out");
  return future.get();
}

std::string env(const char* name) {
  const auto value = std::getenv(name);
  if (!value)
    throw std::runtime_error(std::string("Missing ") + name);
  return value;
}

template <class F>
void eventually(F test) {
  const auto deadline = SteadyClock::now() + 10s;
  while (!test()) {
    check(SteadyClock::now() < deadline, "Data discovery timed out");
    std::this_thread::sleep_for(10ms);
  }
}
}  // namespace

int main() {
  try {
    const bool encrypted = env("LIVEKIT_DATA_ENCRYPTION") == "1";
    auto tx = require_result(create_livekit_room()), rx = require_result(create_livekit_room());
    auto sending = require_result(tx->services()), receiving = require_result(rx->services());
    app_support::DataEventMailbox data_events(receiving);
    receiving->register_rpc("echo", [](RpcInvocation call, RpcCompletion complete) {
      check(call.caller_identity == "publisher", "RPC caller identity lost");
      complete({"reply:" + call.payload, {}});
    });
    auto stream_data = std::make_shared<std::promise<std::string>>();
    auto stream_result = stream_data->get_future();
    receiving->register_stream(true, "documents", [stream_data](auto reader) {
      try {
        check(reader->info().sender_identity == "publisher", "Stream sender identity lost");
        stream_data->set_value(reader->read_all_text());
      } catch (...) {
        stream_data->set_exception(std::current_exception());
      }
    });
    LiveKitRoomOptions options;
    options.url = env("LIVEKIT_URL");
    options.reconnect.enabled = false;
    if (encrypted) {
      auto keys = std::make_shared<ExternalKeyProvider>();
      keys->set_passphrase(0, "room-data-contract");
      options.encryption.keys = keys;
    }
    options.token = env("LIVEKIT_PUBLISH_TOKEN");
    await(tx->join(options));
    std::shared_ptr<ExternalKeyProvider> receiver_keys;
    if (encrypted) {
      receiver_keys = std::make_shared<ExternalKeyProvider>();
      receiver_keys->set_passphrase(0, "wrong-data-key");
      options.encryption.keys = receiver_keys;
    }
    options.token = env("LIVEKIT_SUBSCRIBE_TOKEN");
    await(rx->join(options));
    if (encrypted) {
      await(sending->send_data({0xfe}));
      auto failure = data_events.take_event(5s);
      check(failure && std::holds_alternative<DataServiceError>(failure->value),
            "Wrong data key did not fail closed");
      receiver_keys->set_passphrase(0, "room-data-contract");
    }
    std::cerr << "data rooms joined\n";
    for (bool reliable : {true, false}) {
      DataSendOptions send;
      send.reliable = reliable;
      send.topic = "sample";
      send.destinations = {"subscriber"};
      await(sending->send_data({1, 3, 5, 7}, send));
      auto event = data_events.take_event(5s);
      check(event && event->sender_identity == "publisher" && event->reliable == reliable,
            "Data routing metadata lost");
      const auto* data = std::get_if<ReceivedData>(&event->value);
      check(data && data->topic == "sample" && data->bytes == std::vector<uint8_t>({1, 3, 5, 7}),
            "Data payload changed");
    }
    await(sending->send_dtmf({5, "5"}));
    auto digit = data_events.take_event(5s);
    check(digit && std::holds_alternative<DtmfDigit>(digit->value) &&
              std::get<DtmfDigit>(digit->value).code == 5,
          "DTMF not delivered");
    Transcription transcript;
    transcript.participant_identity = "speaker";
    transcript.track_id = "source";
    transcript.segments.push_back({"segment", "hello", 1, 2, true, "en"});
    await(sending->send_transcription(transcript));
    auto speech = data_events.take_event(5s);
    check(speech && speech->sender_identity == "publisher" &&
              std::holds_alternative<Transcription>(speech->value) &&
              std::get<Transcription>(speech->value).participant_identity == "speaker",
          "Transcription subject/sender confused");
    RpcCallOptions call;
    call.destination_identity = "subscriber";
    call.method = "echo";
    call.payload = "encoded-room";
    check(await(sending->call_rpc(call)) == "reply:encoded-room", "RPC did not round trip");
    std::cerr << "data and RPC passed\n";
    std::string body(100000, 'x');
    body += "UTF-8: 世界";
    StreamOptions stream;
    stream.topic = "documents";
    stream.destination_identities = {"subscriber"};
    await(sending->send_text(body, stream));
    check(await(std::move(stream_result)) == body, "Chunked stream content changed");
    std::cerr << "stream passed\n";

    auto a = await(sending->publish_data_track("telemetry-a"));
    auto b = await(sending->publish_data_track("telemetry-b"));
    std::vector<RemoteDataTrack> remote;
    eventually([&] {
      remote = receiving->remote_data_tracks();
      return remote.size() == 2;
    });
    std::shared_ptr<DataTrackStream> ar, br;
    for (const auto& track : remote) {
      auto reader = await(track.subscribe());
      check(track.info().encrypted == encrypted, "DataTrack encryption metadata changed");
      if (track.info().name == "telemetry-a")
        ar = reader;
      else if (track.info().name == "telemetry-b")
        br = reader;
    }
    check(ar && br, "DataTrack discovery lost names");
    const std::vector<uint8_t> large(6000, 42);
    a.write({large, 123});
    b.write({{2, 4, 6}, 456});
    auto af = ar->read(5s), bf = br->read(5s);
    check(af && af->payload == large && af->user_timestamp == 123,
          "Fragmented DataTrack payload changed");
    check(bf && bf->payload == std::vector<uint8_t>({2, 4, 6}) && bf->user_timestamp == 456,
          "Independent DataTrack routed incorrectly");
    await(a.unpublish());
    b.write({{8, 9}, 789});
    bf = br->read(5s);
    check(bf && bf->payload == std::vector<uint8_t>({8, 9}), "Removing DataTrack stopped sibling");
    std::stop_source stop;
    stop.request_stop();
    DataSendOptions cancelled;
    cancelled.operation.stop = stop.get_token();
    bool rejected = false;
    try {
      await(sending->send_data({1}, cancelled));
    } catch (const SDKError& error) {
      rejected = error.code() == ErrorCode::cancelled;
    }
    check(rejected, "Cancelled data send accepted");
    await(tx->leave());
    await(rx->leave());
    rejected = false;
    try {
      await(sending->send_data({1}));
    } catch (const SDKError&) {
      rejected = true;
    }
    check(rejected, "Service handle survived room close as an active transport");
    std::cout
        << "{\"data\":true,\"rpc\":true,\"stream\":true,\"multiDataTrack\":true,\"encrypted\":"
        << (encrypted ? "true" : "false") << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
