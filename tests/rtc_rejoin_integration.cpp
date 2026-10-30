#include "rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
#include <livekit/rtc/livekit.hpp>
#include <livekit/auth.hpp>
#include "rtc_video_fixtures.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;
namespace sdk = livekit::sdk;

void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

template <class F>
void until(F predicate, const char* message) {
  const auto deadline = SteadyClock::now() + 20s;
  while (!predicate()) {
    check(SteadyClock::now() < deadline, message);
    std::this_thread::sleep_for(10ms);
  }
}

int main(int argc, char** argv) {
  try {
    check(argc == 4, "Usage: rtc_rejoin_integration server-url proxy-url marker-directory");
    const std::filesystem::path markers(argv[3]);
    auto token = [](const char* identity) {
      return sdk::make_token(
          "reconnect-test", "reconnect-integration-secret-32-characters", identity,
          {{"roomJoin", true}, {"room", "rejoin"}, {"canPublish", true}, {"canSubscribe", true}});
    };
    auto tx = require_result(create_livekit_room()), rx = require_result(create_livekit_room());
    livekit::sdk::app_support::RtcEventMailbox rx_events(*rx);
    LiveKitRoomOptions options;
    options.url = argv[1];
    options.token = token("observer");
    options.reconnect.enabled = false;
    require_result(rx->join(options));
    options.url = argv[2];
    options.token = token("publisher");
    options.reconnect.enabled = true;
    options.reconnect.initial_delay = 100ms;
    require_result(tx->join(options));
    std::vector<std::shared_ptr<PublishedTrack>> publications;
    for (int i = 0; i < 3; ++i) {
      TrackDescriptor d;
      d.id = d.name = "camera-" + std::to_string(i);
      d.format.codec = Codec::vp8;
      d.format.width = 64;
      d.format.height = 48;
      d.source = PublicationSource::camera;
      d.stream_group = "stable-stream";
      publications.push_back(require_result(tx->publish(d)));
    }
    require_result(publications.back()->close());
    publications.pop_back();
    auto removed = require_result(tx->publish([] {
      TrackDescriptor d;
      d.id = "removed";
      d.format.codec = Codec::vp8;
      d.format.width = 64;
      d.format.height = 48;
      return d;
    }()));
    require_result(removed->close());
    TrackDescriptor audio;
    audio.id = "muted-audio";
    audio.kind = MediaKind::audio;
    audio.format.codec = Codec::opus;
    audio.format.clock_rate = 48000;
    audio.format.channels = 2;
    audio.muted = true;
    auto muted = require_result(tx->publish(audio));
    std::vector<std::shared_ptr<SubscribedTrack>> subscriptions(2);
    auto discover = [&] {
      while (auto event = rx_events.take_event(0ms)) {
        if (event->kind != RoomEventKind::remote_track_added)
          continue;
        for (int i = 0; i < 2; ++i)
          if (event->descriptor->name == "camera-" + std::to_string(i)) {
            if (subscriptions[i])
              require_result(subscriptions[i]->close());
            subscriptions[i] = require_result(rx->subscribe(*event->descriptor));
          }
      }
    };
    uint64_t sequence = 0, frames = 0;
    auto media = [&] {
      int received[2] = {};
      until(
          [&] {
            discover();
            for (int i = 0; i < 2; ++i) {
              const auto payload = fixture_video(Codec::vp8, i != 0);
              EncodedFrame frame;
              frame.buffer = EncodedBuffer(payload);
              frame.timestamp = sequence * 3000;
              frame.duration = 33333us;
              frame.keyframe = true;
              frame.epoch = publications[i]->snapshot().epoch;
              const auto written = publications[i]->try_write(std::move(frame));
              check(written == WriteStatus::accepted || written == WriteStatus::backpressure,
                    "Restored publication rejected frame");
              if (subscriptions[i])
                while (true) {
                  auto read = subscriptions[i]->read(0ms);
                  if (read.status != ReadStatus::frame)
                    break;
                  check(std::ranges::equal(read.frame->buffer.bytes(), payload),
                        "Rejoin crossed media routes");
                  ++received[i];
                  ++frames;
                }
            }
            ++sequence;
            return received[0] >= 5 && received[1] >= 5;
          },
          "Rejoined media missing");
    };
    media();
    auto previous = tx->snapshot();
    std::vector<std::string> sids{publications[0]->snapshot().server_sid,
                                  publications[1]->snapshot().server_sid};
    for (int round = 1; round <= 2; ++round) {
      std::ofstream(markers / ("rejoin-" + std::to_string(round))) << "rejoin";
      until(
          [&] {
            const auto state = tx->snapshot();
            check(state.state != ConnectionState::failed, state.error.c_str());
            return state.epoch > previous.epoch && state.state == ConnectionState::connected &&
                   publications[0]->snapshot().state == TrackState::ready &&
                   publications[1]->snapshot().state == TrackState::ready &&
                   muted->snapshot().state == TrackState::ready;
          },
          "Full rejoin timed out");
      const auto current = tx->snapshot();
      check(current.session && current.session->local.sid != previous.session->local.sid,
            "Participant SID was not replaced");
      check(current.publications == 3 && removed->snapshot().state == TrackState::closed,
            "Removed publication resurrected");
      check(muted->snapshot().muted, "Mute intent lost on rejoin");
      for (int i = 0; i < 2; ++i) {
        const auto track = publications[i]->snapshot();
        check(track.server_sid != sids[i] && track.epoch == current.epoch,
              "Publication binding was not refreshed");
        EncodedFrame stale;
        stale.buffer = EncodedBuffer(fixture_video(Codec::vp8, i != 0));
        stale.epoch = previous.epoch;
        stale.keyframe = true;
        check(publications[i]->try_write(std::move(stale)) != WriteStatus::accepted,
              "Previous epoch frame accepted");
        sids[i] = track.server_sid;
      }
      media();
      previous = current;
    }
    std::ofstream(markers / "terminal") << "remove";
    until([&] { return tx->snapshot().state == ConnectionState::failed; },
          "Terminal removal not handled");
    check(tx->snapshot().epoch == previous.epoch, "Terminal removal triggered rejoin");
    require_result(rx->leave());
    require_result(tx->leave());
    std::cout << sdk::Json{{"ok", true},
                           {"rejoins", 2},
                           {"frames", frames},
                           {"multiTrackIsolation", true},
                           {"stableSourceIds", true},
                           {"staleEpochRejected", true},
                           {"muteRestored", true},
                           {"removedTrackNotRepublished", true},
                           {"terminalLeave", true}}
                     .dump()
              << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
