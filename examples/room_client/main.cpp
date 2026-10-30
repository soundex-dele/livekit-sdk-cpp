#include "rtc_blocking_bridge.hpp"
#include "../../support/rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "livekit/rtc/livekit.hpp"
#include <livekit/rtc/engine.hpp>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace rtc = livekit::sdk::rtc;
using namespace std::chrono_literals;

namespace {
template <class Future>
auto complete(livekit::sdk::app_support::BlockingRtc&, Future future) {
  return require_result(std::move(future));
}

class EncodedConsumer : public graphflow::core::GraphNode {
 public:
  explicit EncodedConsumer(std::string id)
      : GraphNode("encoded-consumer", nullptr), id_(std::move(id)) {}

  std::atomic<size_t> frames{0}, bytes{0};

 protected:
  bool doInit() override {
    subscribe<rtc::nodes::EncodedFrameEvent>([this](const auto& event) {
      // event.frame owns the complete encoded access unit. Copy the frame to
      // retain it or send it to another bounded graph/processing queue.
      ++frames;
      bytes += event.frame.buffer.size();
      if (frames == 1)
        std::cout << id_ << ": first encoded frame, " << event.frame.buffer.size() << " bytes\n";
    });
    return true;
  }

 private:
  std::string id_;
};
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr
        << "Usage: livekit_sdk_example URL [publish|subscribe] [SECONDS]; set LIVEKIT_TOKEN\n";
    return 2;
  }
  try {
    auto engine = require_result(livekit::sdk::app_support::create_blocking_engine());
    livekit::sdk::app_support::RtcEventMailbox engine_events(*engine);
    rtc::LiveKitRoomOptions options;
    options.url = argv[1];
    if (const auto token = std::getenv("LIVEKIT_TOKEN"))
      options.token = token;
    complete(*engine, engine->join(options));
    const bool publishing = argc > 2 && std::string(argv[2]) == "publish";
    const auto duration = std::chrono::seconds(argc > 3 ? std::stoi(argv[3]) : 10);
    std::vector<rtc::TrackId> tracks;
    std::vector<std::shared_ptr<EncodedConsumer>> consumers;
    if (publishing)
      for (int i = 0; i < 2; ++i) {
        rtc::TrackDescriptor descriptor;
        descriptor.id = descriptor.name = "audio-" + std::to_string(i);
        descriptor.kind = rtc::MediaKind::audio;
        descriptor.format.codec = rtc::Codec::opus;
        descriptor.format.clock_rate = 48000;
        descriptor.format.channels = 2;
        tracks.push_back(complete(*engine, engine->publish(descriptor)));
      }
    const auto end = rtc::SteadyClock::now() + duration;
    uint64_t timestamp = 0;
    while (rtc::SteadyClock::now() < end) {
      while (auto event = engine_events.take_event(0ms)) {
        if (event->kind == rtc::RoomEventKind::failed)
          throw std::runtime_error(event->message);
        if (!publishing && event->kind == rtc::RoomEventKind::remote_track_added) {
          auto sink = std::make_shared<EncodedConsumer>(event->descriptor->name);
          require_result(engine->graph().subscribe(*event->descriptor, sink));
          consumers.push_back(std::move(sink));
        }
      }
      for (const auto& id : tracks) {
        rtc::EncodedFrame frame;
        // Pre-encoded 20 ms Opus silence. Real applications supply encoded
        // output from an upstream node; the RTC backend never encodes PCM.
        frame.buffer = rtc::EncodedBuffer({0xf8, 0xff, 0xfe});
        frame.epoch = engine->publication(id).epoch;
        frame.timestamp = timestamp;
        frame.duration = 20ms;
        const auto status = engine->write(id, std::move(frame));
        if (status == rtc::WriteStatus::invalid_frame || status == rtc::WriteStatus::closed)
          throw std::runtime_error("Publication rejected encoded media");
      }
      timestamp += 960;

      while (auto failure = engine_events.take_failure())
        std::cerr << failure->id << ": " << failure->message << '\n';
      std::this_thread::sleep_for(20ms);
    }
    require_result(engine->close());
    for (const auto& sink : consumers)
      std::cout << "Received " << sink->frames << " encoded frames, " << sink->bytes << " bytes\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
