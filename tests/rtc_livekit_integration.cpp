#include "rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
#include "livekit/rtc/livekit.hpp"
#include "livekit/error.hpp"
#include "rtc_video_fixtures.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <thread>

using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

namespace {
uint64_t sfu_silence_frames = 0;

void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

template <class T>
auto await(T result) {
  return require_result(std::move(result));
}

template <class T>
auto await(std::future<T> future) {
  check(future.wait_for(15s) == std::future_status::ready, "RTC operation timed out");
  return future.get();
}

template <class F>
void eventually(F check_state, const char* message) {
  const auto deadline = SteadyClock::now() + 10s;
  while (!check_state()) {
    check(SteadyClock::now() < deadline, message);
    std::this_thread::sleep_for(10ms);
  }
}

std::string env(const char* key) {
  const auto value = std::getenv(key);
  if (!value || !*value)
    throw std::runtime_error(std::string("Missing ") + key);
  return value;
}

bool media_encrypted() {
  const auto value = std::getenv("LIVEKIT_MEDIA_ENCRYPTION");
  return value && std::string(value) == "1";
}

Codec video_codec() {
  return media_encrypted() ? Codec::h264 : Codec::vp8;
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
    } else
      result.push_back(bytes[i++]);
  }
  return result;
}

TrackDescriptor descriptor(int index) {
  TrackDescriptor d;
  d.id = d.name = "stream-" + std::to_string(index);
  d.muted = index == 0;
  d.kind = index < 2 ? MediaKind::audio : MediaKind::video;
  d.source = index == 0   ? PublicationSource::microphone
             : index == 1 ? PublicationSource::screen_audio
             : index == 2 ? PublicationSource::camera
                          : PublicationSource::screen;
  d.format.codec = index < 2 ? Codec::opus : video_codec();
  d.format.clock_rate = index < 2 ? 48000 : 90000;
  d.format.channels = index < 2 ? 2 : 0;
  d.format.width = index < 2 ? 0 : 64;
  d.format.height = index < 2 ? 0 : 48;
  return d;
}

std::vector<uint8_t> payload(int index) {
  return index < 2 ? std::vector<uint8_t>{0xf8, 0xff, 0xfe}
                   : fixture_video(video_codec(), index == 3);
}

void transmit(const std::vector<std::shared_ptr<PublishedTrack>>& tx,
              const std::vector<std::shared_ptr<SubscribedTrack>>& rx,
              const std::vector<int>& indices, uint64_t& sequence, bool resubscribed = false,
              std::source_location caller = std::source_location::current()) {
  std::vector<int> received(indices.size());
  const auto deadline = SteadyClock::now() + 10s;
  while (SteadyClock::now() < deadline) {
    for (size_t n = 0; n < indices.size(); ++n) {
      const int i = indices[n];
      EncodedFrame f;
      f.buffer = EncodedBuffer(payload(i));
      f.epoch = tx[i]->snapshot().epoch;
      f.timestamp = sequence * (i < 2 ? 960 : 1800);
      f.duration = 20ms;
      f.keyframe = i >= 2;
      const auto status = tx[i]->try_write(std::move(f));
      check(status == WriteStatus::accepted || status == WriteStatus::backpressure,
            "Media write rejected");
      while (true) {
        const auto read = rx[i]->read(0ms);
        if (read.status != ReadStatus::frame)
          break;
        const auto expected = payload(i);
        // LiveKit flushes a closing VP8 downtrack with this documented 8x8
        // keyframe. A rapid resubscribe can receive the in-flight SFU flush;
        // it is valid encoded media, not an application frame for this test.
        static const std::vector<uint8_t> blank = {0x10, 0x02, 0x00, 0x9d, 0x01, 0x2a, 0x08, 0x00,
                                                   0x08, 0x00, 0x00, 0x47, 0x08, 0x85, 0x85, 0x88,
                                                   0x85, 0x84, 0x88, 0x02, 0x02, 0x00, 0x0c, 0x0d,
                                                   0x60, 0x00, 0xfe, 0xff, 0xab, 0x50, 0x80};
        if (resubscribed && i == 2 && std::ranges::equal(read.frame->buffer.bytes(), blank))
          continue;
        // This supplied SFU injects exactly downtrack.go's 80-byte
        // OpusSilenceFrame on publisher mute (f8 ff fe then 77 zero bytes).
        // Only this known filler, on the muted audio track after transition,
        // is excluded from application payload checks. E2EE SIF is suppressed
        // by the decryptor and must not emerge as unauthenticated plaintext.
        const auto bytes = read.frame->buffer.bytes();
        if (resubscribed && i == 0 && !media_encrypted() && bytes.size() == 80 &&
            bytes[0] == 0xf8 && bytes[1] == 0xff && bytes[2] == 0xfe &&
            std::all_of(bytes.begin() + 3, bytes.end(), [](auto value) { return value == 0; })) {
          ++sfu_silence_frames;
          continue;
        }
        if (!(i >= 2 && video_codec() != Codec::vp8
                  ? normalized(read.frame->buffer.bytes()) == normalized(expected)
                  : std::ranges::equal(read.frame->buffer.bytes(), expected))) {
          std::cerr << "payload mismatch track=" << i << " size=" << read.frame->buffer.size()
                    << " expected=" << expected.size() << " bytes=";
          for (auto byte : read.frame->buffer.bytes())
            std::cerr << std::hex << unsigned(byte) << ',';
          std::cerr << std::dec << '\n';
          throw std::runtime_error("Received payload routed to wrong track or altered");
        }
        check(read.frame->epoch == rx[i]->snapshot().epoch, "Received stale epoch");
        ++received[n];
      }
    }
    ++sequence;
    if (std::all_of(received.begin(), received.end(), [](int n) { return n >= 3; }))
      return;
    std::this_thread::sleep_for(20ms);
  }
  std::cerr << "Transmit timed out at integration line " << caller.line() << '\n';
  for (size_t n = 0; n < indices.size(); ++n)
    std::cerr << "track " << indices[n] << " received " << received[n] << '\n';
  throw std::runtime_error("Encoded media did not cross SFU");
}

void verify_receive_pause(const std::vector<std::shared_ptr<PublishedTrack>>& tx,
                          const std::vector<std::shared_ptr<SubscribedTrack>>& rx,
                          uint64_t& sequence) {
  // Continue publishing both video tracks. Allow the SFU's settings debounce
  // and in-flight packets to settle, then require a sustained quiet interval.
  const auto start = SteadyClock::now();
  size_t sibling_frames = 0;
  while (SteadyClock::now() - start < 1800ms) {
    for (const int i : {2, 3}) {
      EncodedFrame frame;
      frame.buffer = EncodedBuffer(payload(i));
      frame.epoch = tx[i]->snapshot().epoch;
      frame.timestamp = sequence * 1800;
      frame.duration = 20ms;
      frame.keyframe = true;
      const auto status = tx[i]->try_write(std::move(frame));
      check(status == WriteStatus::accepted || status == WriteStatus::backpressure,
            "Paused subscriber stopped publisher admission");
      while (rx[i]->read(0ms).status == ReadStatus::frame) {
        if (SteadyClock::now() - start > 1s) {
          check(i != 3, "SFU still forwarded disabled subscription");
          ++sibling_frames;
        }
      }
    }
    ++sequence;
    std::this_thread::sleep_for(20ms);
  }
  check(sibling_frames >= 3, "Receive pause stopped sibling forwarding");
}
}  // namespace

int main() {
  try {
    auto publisher = require_result(create_livekit_room()),
         subscriber = require_result(create_livekit_room());
    livekit::sdk::app_support::RtcEventMailbox subscriber_events(*subscriber);
    livekit::sdk::app_support::RtcEventMailbox publisher_events(*publisher);
    LiveKitRoomOptions options;
    options.url = env("LIVEKIT_URL");
    options.reconnect.enabled = false;
    options.signal_protocol = 15;
    std::shared_ptr<livekit::sdk::ExternalKeyProvider> rx_keys;
    if (media_encrypted()) {
      auto keys = std::make_shared<livekit::sdk::ExternalKeyProvider>();
      keys->set_passphrase(0, "encoded-media-secret");
      options.encryption.keys = keys;
      options.encryption.media = true;
    }
    SubscriptionPermissions policy{false,
                                   {{"subscriber", false, {"stream-0", "stream-1", "stream-2"}}}};
    options.subscription_permissions = policy;
    options.token = env("LIVEKIT_PUBLISH_TOKEN");
    await(publisher->join(options));
    options.subscription_permissions.reset();
    if (media_encrypted()) {
      rx_keys = std::make_shared<livekit::sdk::ExternalKeyProvider>();
      rx_keys->set_passphrase(0, "wrong-media-secret");
      options.encryption.keys = rx_keys;
    }
    options.token = env("LIVEKIT_SUBSCRIBE_TOKEN");
    await(subscriber->join(options));
    const auto local = publisher->snapshot().session;
    check(local && local->epoch == publisher->snapshot().epoch &&
              local->room.name == "encoded-backend" && local->local.identity == "publisher" &&
              local->local.permissions && local->local.permissions->can_publish,
          "Join did not expose room/local/permission state");
    const auto publisher_sid = local->local.sid;
    eventually(
        [&] {
          const auto state = subscriber->snapshot().session;
          return state && state->remote.contains(publisher_sid) &&
                 state->remote.at(publisher_sid).identity == "publisher";
        },
        "Subscriber roster missed publisher");
    await(publisher->update_local_participant(
        {"renamed publisher", "updated metadata", {{"role", "presenter"}}}));
    eventually(
        [&] {
          const auto state = subscriber->snapshot().session;
          const auto self = publisher->snapshot().session;
          const auto& remote = state->remote.at(publisher_sid);
          return remote.name == "renamed publisher" && remote.metadata == "updated metadata" &&
                 remote.attributes.contains("role") &&
                 remote.attributes.at("role") == "presenter" && self->local.name == remote.name &&
                 self->local.metadata == remote.metadata;
        },
        "Acknowledged participant update did not reach local/remote state");
    await(publisher->update_local_participant({{}, {}, {{"role", ""}}}));
    eventually(
        [&] {
          const auto state = subscriber->snapshot().session;
          const auto& remote = state->remote.at(publisher_sid);
          return !remote.attributes.contains("role") && remote.name == "renamed publisher" &&
                 remote.metadata == "updated metadata";
        },
        "Attribute deletion or unchanged scalar semantics failed");
    bool denied = false;
    try {
      await(subscriber->update_local_participant({"forbidden", {}, {}}));
    } catch (const livekit::sdk::SDKError& error) {
      denied = error.code() == livekit::sdk::ErrorCode::permission_denied;
    }
    check(denied, "Metadata update ignored server permission rejection");
    std::cerr << "joined both rooms\n";
    std::vector<std::shared_ptr<PublishedTrack>> tx;
    for (int i = 0; i < 4; ++i) {
      tx.push_back(await(publisher->publish(descriptor(i))));
      std::cerr << "published " << i << '\n';
    }
    std::vector<TrackDescriptor> remote(4);
    size_t discovered = 0;
    const auto deadline = SteadyClock::now() + 10s;
    while (discovered != 4 && SteadyClock::now() < deadline) {
      auto event = subscriber_events.take_event(100ms);
      if (!event || event->kind != RoomEventKind::remote_track_added)
        continue;
      for (int i = 0; i < 4; ++i)
        if (event->descriptor->name == descriptor(i).name && remote[i].id.empty()) {
          remote[i] = *event->descriptor;
          remote[i].id = "receive-" + std::to_string(i);
          ++discovered;
          check(remote[i].source == descriptor(i).source, "Discovery lost publication source");
        }
    }
    check(discovered == 4, "Remote discovery missed publications");
    eventually(
        [&] {
          const auto state = subscriber->snapshot().session;
          return state && state->remote.at(publisher_sid).publications.size() == 4;
        },
        "Member publications did not follow discovery");
    check(publisher->snapshot().session->local.publications.size() == 4,
          "Local publication state missing");
    for (int i = 0; i < 4; ++i) {
      const auto sid = tx[i]->snapshot().server_sid;
      check(
          publisher->snapshot().session->local.publications.at(sid).source == descriptor(i).source,
          "Local roster lost publication source");
      check(subscriber->snapshot().session->remote.at(publisher_sid).publications.at(sid).source ==
                descriptor(i).source,
            "Remote roster lost publication source");
    }
    const auto first_sid = tx[0]->snapshot().server_sid;
    check(tx[0]->snapshot().muted &&
              publisher->snapshot().session->local.publications.at(first_sid).muted,
          "Initial muted publication was not honored");
    eventually(
        [&] {
          return subscriber->snapshot()
              .session->remote.at(publisher_sid)
              .publications.at(first_sid)
              .muted;
        },
        "Initial mute did not reach remote roster");
    await(tx[0]->set_muted(false));
    eventually(
        [&] {
          return !subscriber->snapshot()
                      .session->remote.at(publisher_sid)
                      .publications.at(first_sid)
                      .muted;
        },
        "Unmute did not reach remote roster");
    while (auto notification = publisher_events.take_event(0ms))
      check(notification->kind != RoomEventKind::remote_track_added,
            "Local publication appeared as remote discovery");
    remote[3].receive_demand.disabled = true;
    std::vector<std::shared_ptr<SubscribedTrack>> rx;
    for (int i = 0; i < 3; ++i) {
      rx.push_back(await(subscriber->subscribe(remote[i])));
      std::cerr << "subscribed " << remote[i].name << '\n';
    }
    auto pending_denied = std::async(
        std::launch::async, [&] { return require_result(subscriber->subscribe(remote[3])); });
    auto permission_change = [&](bool allowed) {
      const auto deadline = SteadyClock::now() + 5s;
      while (SteadyClock::now() < deadline) {
        const auto event = subscriber_events.take_event(100ms);
        if (event && event->permission && event->permission->track_sid == remote[3].remote_sid &&
            event->permission->allowed == allowed) {
          check(event->permission->participant_sid == publisher_sid,
                "Permission update has wrong publisher");
          return;
        }
      }
      throw std::runtime_error("Subscription permission transition not observed");
    };
    permission_change(false);
    check(pending_denied.wait_for(0ms) != std::future_status::ready,
          "Denied subscription unexpectedly bound");
    policy.participants[0].track_ids.push_back("stream-3");
    await(publisher->set_subscription_permissions(policy));
    rx.push_back(await(std::move(pending_denied)));
    permission_change(true);
    uint64_t sequence = 1;
    // The receiver binds even when its initial forwarding demand is disabled.
    // Exercise media behavior after correcting the E2EE key below.
    if (media_encrypted()) {
      for (int attempt = 0; attempt < 40; ++attempt, ++sequence) {
        for (int i = 0; i < 4; ++i) {
          EncodedFrame frame;
          frame.buffer = EncodedBuffer(payload(i));
          frame.epoch = tx[i]->snapshot().epoch;
          frame.timestamp = sequence * (i < 2 ? 960 : 1800);
          frame.duration = 20ms;
          frame.keyframe = i >= 2;
          tx[i]->try_write(std::move(frame));
          check(rx[i]->read(0ms).status != ReadStatus::frame,
                "Unauthenticated media reached the application");
        }
        std::this_thread::sleep_for(20ms);
      }
      bool rejected = false;
      while (auto notification = subscriber_events.take_event(0ms))
        if (notification->encryption && !notification->encryption->ok)
          rejected = true;
      check(rejected, "Media authentication failure was not observable");
      rx_keys->set_passphrase(0, "encoded-media-secret");
    }
    verify_receive_pause(tx, rx, sequence);
    ReceiveDemand demand;
    demand.width = 64;
    demand.height = 48;
    demand.fps = 30;
    demand.priority = 1;
    await(rx[3]->set_demand(demand));
    check(rx[3]->snapshot().receive_demand == demand,
          "Submitted receive demand missing from snapshot");
    transmit(tx, rx, {0, 1, 2, 3}, sequence);
    std::cerr << "four tracks received\n";
    demand.disabled = true;
    await(subscriber->set_receive_demand(remote[3].id, demand));
    verify_receive_pause(tx, rx, sequence);
    demand.disabled = false;
    await(rx[3]->set_demand(demand));
    transmit(tx, rx, {0, 1, 2, 3}, sequence);
    policy.participants[0].track_ids.pop_back();
    await(publisher->set_subscription_permissions(policy));
    permission_change(false);
    verify_receive_pause(tx, rx, sequence);
    policy.participants[0].track_ids.push_back("stream-3");
    await(publisher->set_subscription_permissions(policy));
    permission_change(true);
    transmit(tx, rx, {0, 1, 2, 3}, sequence);
    await(tx[0]->set_muted(true));
    await(publisher->set_muted("stream-2", true));
    eventually(
        [&] {
          const auto session = subscriber->snapshot().session;
          const auto& tracks = session->remote.at(publisher_sid).publications;
          return tracks.at(first_sid).muted && tracks.at(tx[2]->snapshot().server_sid).muted;
        },
        "Audio/video mute not reflected remotely");
    for (const int i : {0, 2}) {
      EncodedFrame blocked;
      blocked.buffer = EncodedBuffer(payload(i));
      blocked.epoch = tx[i]->snapshot().epoch;
      blocked.timestamp = sequence * (i ? 1800 : 960);
      blocked.duration = 20ms;
      blocked.keyframe = i != 0;
      check(tx[i]->try_write(std::move(blocked)) == WriteStatus::not_ready,
            "Muted publication admitted media");
    }
    transmit(tx, rx, {1, 3}, sequence);
    await(tx[0]->set_muted(false));
    await(tx[2]->set_muted(false));
    eventually([&] { return tx[2]->feedback().encodings.at("").active; },
               "Video feedback did not recover after unmute");
    transmit(tx, rx, {0, 1, 2, 3}, sequence, true);
    await(rx[2]->close());
    rx[2] = await(subscriber->subscribe(remote[2]));
    transmit(tx, rx, {0, 1, 2, 3}, sequence, true);
    std::cerr << "resubscription received\n";
    await(tx[0]->close());
    await(tx[2]->close());
    transmit(tx, rx, {1, 3}, sequence);
    auto withdrawn = await(publisher->publish(descriptor(5)));
    await(publisher->unpublish("stream-5"));
    check(withdrawn->snapshot().state == TrackState::closed, "Unpublish returned before closure");
    check(publisher->snapshot().publications == 2, "Unpublish retained intent");
    transmit(tx, rx, {1, 3}, sequence);
    await(publisher->leave());
    check(!publisher->snapshot().session && local->local.identity == "publisher",
          "Closed session retained live state or invalidated history");
    eventually(
        [&] {
          const auto state = subscriber->snapshot().session;
          return state && !state->remote.contains(publisher_sid);
        },
        "Departed participant remained in roster");
    await(subscriber->leave());
    std::cout << "{\"multiTrack\":true,\"resubscribe\":true,\"independentClose\":true,"
                 "\"serializedRemoval\":true,\"sessionRoster\":true,\"participantControl\":true,"
                 "\"muteControl\":true,\"publicationSources\":true,\"receiveDemand\":true,"
                 "\"subscriptionPermissions\":true,\"sfuSilenceFrames\":"
              << sfu_silence_frames << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
