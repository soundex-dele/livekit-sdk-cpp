#include "rtc_blocking_bridge.hpp"
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "livekit/rtc/livekit.hpp"
#include <livekit/rtc/engine.hpp>
#include <cstdlib>
#include <charconv>
#include <iostream>
#include <thread>

namespace rtc = livekit::sdk::rtc;
using namespace std::chrono_literals;

namespace {
int report(const livekit::sdk::Failure& failure) {
  std::cerr << failure->message << '\n';
  return 1;
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
    auto created = livekit::sdk::app_support::create_blocking_engine();
    if (!created) { return report(created.failure_record()); }
    auto engine = *std::move(created);
    livekit::sdk::app_support::RtcEventMailbox engine_events(*engine);
    rtc::LiveKitRoomOptions options;
    options.url = argv[1];
    if (const auto token = std::getenv("LIVEKIT_TOKEN"))
      options.token = token;
    int seconds = 10;
    if (argc > 3) {
      const std::string_view text(argv[3]);
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || seconds <= 0) {
        std::cerr << "SECONDS must be a positive integer\n";
        return 2;
      }
    }
    auto joined = engine->join(options);
    if (joined != livekit::sdk::ErrorCode::ok) {
      std::cerr << "Join failed: " << static_cast<int>(joined) << '\n';
      return 1;
    }
    const bool publishing = argc > 2 && std::string(argv[2]) == "publish";
    const auto duration = std::chrono::seconds(seconds);
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
        auto published = engine->publish(descriptor);
        if (!published) { return report(published.failure_record()); }
        tracks.push_back(*std::move(published));
      }
    const auto end = rtc::SteadyClock::now() + duration;
    uint64_t timestamp = 0;
    while (rtc::SteadyClock::now() < end) {
      while (true) {
        auto received = engine_events.take_event(0ms);
        if (!received) { return report(received.failure_record()); }
        auto event = *std::move(received);
        if (!event) { break; }
        if (event->kind == rtc::RoomEventKind::failed) {
          std::cerr << event->message << '\n';
          return 1;
        }
        if (!publishing && event->kind == rtc::RoomEventKind::remote_track_added) {
          auto sink = std::make_shared<EncodedConsumer>(event->descriptor->name);
          auto subscribed = engine->graph().subscribe(*event->descriptor, sink);
          if (!subscribed) { return report(subscribed.failure_record()); }
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
        if (status == rtc::WriteStatus::invalid_frame || status == rtc::WriteStatus::closed) {
          std::cerr << "Publication rejected encoded media\n";
          return 1;
        }
      }
      timestamp += 960;

      auto failure = engine_events.take_failure();
      if (!failure) { return report(failure.failure_record()); }
      if (*failure) {
        std::cerr << (*failure)->id << ": " << (*failure)->message << '\n';
        return 1;
      }
      std::this_thread::sleep_for(20ms);
    }
    auto closed = engine->close();
    if (closed != livekit::sdk::ErrorCode::ok) {
      std::cerr << "Close failed: " << static_cast<int>(closed) << '\n';
      return 1;
    }
    for (const auto& sink : consumers)
      std::cout << "Received " << sink->frames << " encoded frames, " << sink->bytes << " bytes\n";
    return 0;
}
