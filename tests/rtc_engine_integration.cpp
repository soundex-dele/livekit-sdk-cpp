#include "rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "rtc_video_fixtures.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

namespace {
void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

template <class Future>
auto await(engine::RtcEngine& engine, Future future) {
  return require_result(std::move(future));
}

class Consumer : public graphflow::core::GraphNode {
 public:
  explicit Consumer(std::vector<uint8_t> expected)
      : GraphNode("verify", nullptr), expected_(std::move(expected)) {}

  std::atomic<int> frames{0};
  std::atomic<bool> closing{false};

 protected:
  bool doInit() override {
    subscribe<nodes::EncodedFrameEvent>([this](const auto& event) {
      const auto bytes = event.frame.buffer.bytes();
      if (!std::ranges::equal(bytes, expected_)) {
        // LiveKit downtrack shutdown sends these exact Opus/VP8 flush frames.
        // Permit them only after teardown starts; never count them as delivery.
        static const std::vector<uint8_t> blank = {0x10, 0x02, 0x00, 0x9d, 0x01, 0x2a, 0x08, 0x00,
                                                   0x08, 0x00, 0x00, 0x47, 0x08, 0x85, 0x85, 0x88,
                                                   0x85, 0x84, 0x88, 0x02, 0x02, 0x00, 0x0c, 0x0d,
                                                   0x60, 0x00, 0xfe, 0xff, 0xab, 0x50, 0x80};
        const bool silence =
            expected_.size() == 3 && bytes.size() == 80 && bytes[0] == 0xf8 && bytes[1] == 0xff &&
            bytes[2] == 0xfe &&
            std::all_of(bytes.begin() + 3, bytes.end(), [](auto byte) { return byte == 0; });
        if (closing && (silence || (expected_.size() > 3 && std::ranges::equal(bytes, blank)))) {
          return;
        }
        throw std::runtime_error("Engine routed incorrect encoded payload");
      }
      ++frames;
    });
    return true;
  }

 private:
  std::vector<uint8_t> expected_;
};
}  // namespace

int main() {
  try {
    auto tx = require_result(engine::create_livekit_engine()),
         rx = require_result(engine::create_livekit_engine());
    livekit::sdk::app_support::RtcEventMailbox rx_events(*rx);
    livekit::sdk::app_support::RtcEventMailbox tx_events(*tx);
    LiveKitRoomOptions options;
    options.url = std::getenv("LIVEKIT_URL");
    options.reconnect.enabled = false;
    options.token = std::getenv("LIVEKIT_PUBLISH_TOKEN");
    await(*tx, tx->join(options));
    options.token = std::getenv("LIVEKIT_SUBSCRIBE_TOKEN");
    await(*rx, rx->join(options));
    std::vector<TrackDescriptor> descriptors;
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<TrackId> publishing;
    for (int i = 0; i < 4; ++i) {
      TrackDescriptor d;
      d.id = d.name = "graph-" + std::to_string(i);
      d.kind = i < 2 ? MediaKind::audio : MediaKind::video;
      d.format.codec = i < 2 ? Codec::opus : Codec::vp8;
      d.format.clock_rate = i < 2 ? 48000 : 90000;
      d.format.channels = i < 2 ? 2 : 0;
      d.format.width = i < 2 ? 0 : 64;
      d.format.height = i < 2 ? 0 : 48;
      descriptors.push_back(d);
      payloads.push_back(i < 2 ? std::vector<uint8_t>{0xf8, 0xff, 0xfe}
                               : fixture_video(Codec::vp8, i == 3));
      publishing.push_back(tx->publish(d));
    }
    for (auto& future : publishing)
      await(*tx, std::move(future));
    std::vector<std::shared_ptr<Consumer>> sinks(4);
    std::vector<TrackId> subscribing;
    const auto discovered_by = SteadyClock::now() + 10s;
    while (subscribing.size() < 4) {
      check(SteadyClock::now() < discovered_by, "Engine remote discovery timed out");
      auto event = rx_events.take_event(100ms);
      if (!event || event->kind != RoomEventKind::remote_track_added)
        continue;
      for (int i = 0; i < 4; ++i)
        if (event->descriptor->name == descriptors[i].name && !sinks[i]) {
          sinks[i] = std::make_shared<Consumer>(payloads[i]);
          auto d = *event->descriptor;
          d.id = descriptors[i].id;
          subscribing.push_back(rx->graph().subscribe(d, sinks[i]));
        }
    }
    for (auto& future : subscribing)
      await(*rx, std::move(future));
    uint64_t sequence = 0;
    auto run = [&](std::vector<int> active) {
      for (int i : active)
        sinks[i]->frames = 0;
      const auto deadline = SteadyClock::now() + 10s;
      while (true) {
        check(SteadyClock::now() < deadline, "Graph media delivery timed out");
        ++sequence;
        for (int i : active) {
          EncodedFrame f;
          f.buffer = EncodedBuffer(payloads[i]);
          f.epoch = tx->publication(descriptors[i].id).epoch;
          f.timestamp = sequence * (i < 2 ? 960 : 1800);
          f.duration = 20ms;
          f.keyframe = i >= 2;
          auto status = tx->write(descriptors[i].id, std::move(f));
          check(status == WriteStatus::accepted || status == WriteStatus::backpressure,
                "Graph rejected media");
        }
        // Graph setup has completed. Media and feedback must progress while
        // both control threads deliberately stop polling.
        if (auto failure = tx_events.take_failure())
          throw std::runtime_error(failure->message);
        if (auto failure = rx_events.take_failure())
          throw std::runtime_error(failure->message);
        if (std::all_of(active.begin(), active.end(), [&](int i) { return sinks[i]->frames >= 3; }))
          return;
        std::this_thread::sleep_for(20ms);
      }
    };
    run({0, 1, 2, 3});
    await(*rx, rx->unsubscribe("graph-0"));
    await(*rx, rx->unsubscribe("graph-2"));
    await(*tx, tx->unpublish("graph-0"));
    await(*tx, tx->unpublish("graph-2"));
    run({1, 3});
    // Reuse the same facade with fresh native transport and service state.
    const auto previous_epoch = tx->room().snapshot().epoch;
    auto old_services = require_result(tx->room().services());
    for (auto& sink : sinks) {
      sink->closing = true;
    }
    require_result(tx->leave());
    require_result(rx->leave());
    options.token = std::getenv("LIVEKIT_PUBLISH_TOKEN");
    await(*tx, tx->join(options));
    options.token = std::getenv("LIVEKIT_SUBSCRIBE_TOKEN");
    await(*rx, rx->join(options));
    check(tx->room().snapshot().epoch > previous_epoch, "Native rejoin reused its epoch");
    check(old_services != require_result(tx->room().services()),
          "Native rejoin reused closed room services");
    bool old_service_closed = false;
    try {
      old_services->send_data({1}).get();
    } catch (const std::exception&) {
      old_service_closed = true;
    }
    check(old_service_closed, "Old room services affected the new join");
    require_result(tx->room().services())->send_data({2}).get();
    await(*tx, tx->publish(descriptors[3]));
    const auto rejoined_by = SteadyClock::now() + 10s;
    bool subscribed_again = false;
    while (!subscribed_again) {
      check(SteadyClock::now() < rejoined_by, "Rejoined remote discovery timed out");
      auto event = rx_events.take_event(100ms);
      if (!event || event->kind != RoomEventKind::remote_track_added ||
          event->descriptor->name != descriptors[3].name) {
        continue;
      }
      sinks[3] = std::make_shared<Consumer>(payloads[3]);
      auto descriptor = *event->descriptor;
      descriptor.id = descriptors[3].id;
      await(*rx, rx->graph().subscribe(descriptor, sinks[3]));
      subscribed_again = true;
    }
    run({3});
    // close() returns after graph delivery and native detachment barriers.
    sinks[3]->closing = true;
    require_result(tx->close());
    require_result(rx->close());
    std::cout << "{\"engineMultiGraph\":true,\"encodedMedia\":true,\"independentClose\":true,"
                 "\"leaveRejoin\":true,\"serviceIsolation\":true,\"noMediaPoll\":true}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
