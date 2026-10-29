#include <livekit/rtc/engine.hpp>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace rtc = livekit::sdk::rtc;
using namespace std::chrono_literals;
namespace {
template<class T> auto complete(rtc::engine::RtcEngine& engine, std::future<T> future) {
  while (future.wait_for(0ms) != std::future_status::ready) {
    engine.poll(); std::this_thread::sleep_for(2ms);
  }
  return future.get();
}
class EncodedConsumer : public graphflow::core::GraphNode {
 public:
  explicit EncodedConsumer(std::string id) : GraphNode("encoded-consumer", nullptr), id_(std::move(id)) {}
  size_t frames = 0, bytes = 0;
 protected:
  bool doInit() override {
    subscribe<rtc::nodes::EncodedFrameEvent>([this](const auto& event) {
      // event.frame owns the complete encoded access unit. Copy the frame to
      // retain it or send it to another bounded graph/processing queue.
      ++frames; bytes += event.frame.buffer.size();
      if (frames == 1) std::cout << id_ << ": first encoded frame, " << event.frame.buffer.size() << " bytes\n";
    });
    return true;
  }
 private:
  std::string id_;
};
}
int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: livekit_sdk_example URL [publish|subscribe] [SECONDS]; set LIVEKIT_TOKEN\n";
    return 2;
  }
  try {
    auto engine = rtc::engine::create_livekit_engine();
    rtc::RoomOptions options; options.url = argv[1];
    if (const auto token = std::getenv("LIVEKIT_TOKEN")) options.token = token;
    complete(*engine, engine->join(options));
    const bool publishing = argc > 2 && std::string(argv[2]) == "publish";
    const auto duration = std::chrono::seconds(argc > 3 ? std::stoi(argv[3]) : 10);
    std::vector<rtc::TrackId> tracks;
    std::vector<std::shared_ptr<EncodedConsumer>> consumers;
    std::vector<std::future<rtc::TrackId>> subscriptions;
    if (publishing) for (int i = 0; i < 2; ++i) {
      rtc::TrackDescriptor descriptor;
      descriptor.id = descriptor.name = "audio-" + std::to_string(i);
      descriptor.kind = rtc::MediaKind::audio;
      descriptor.format.codec = rtc::Codec::opus;
      descriptor.format.clock_rate = 48000; descriptor.format.channels = 2;
      tracks.push_back(complete(*engine, engine->publish(descriptor)));
    }
    const auto end = rtc::SteadyClock::now() + duration;
    uint64_t timestamp = 0;
    while (rtc::SteadyClock::now() < end) {
      while (auto event = engine->room().next_event(0ms)) {
        if (event->kind == rtc::RoomEventKind::failed) throw std::runtime_error(event->message);
        if (!publishing && event->kind == rtc::RoomEventKind::remote_track_added) {
          auto sink = std::make_shared<EncodedConsumer>(event->descriptor->name);
          subscriptions.push_back(engine->subscribe(*event->descriptor, sink));
          consumers.push_back(std::move(sink));
        }
      }
      for (auto it = subscriptions.begin(); it != subscriptions.end();) {
        if (it->wait_for(0ms) == std::future_status::ready) { it->get(); it = subscriptions.erase(it); }
        else ++it;
      }
      for (const auto& id : tracks) {
        rtc::EncodedFrame frame;
        // Pre-encoded 20 ms Opus silence. Real applications supply encoded
        // output from an upstream node; the RTC backend never encodes PCM.
        frame.buffer = rtc::EncodedBuffer({0xf8, 0xff, 0xfe});
        frame.epoch = engine->publication(id).epoch;
        frame.timestamp = timestamp; frame.duration = 20ms;
        const auto status = engine->write(id, std::move(frame));
        if (status == rtc::WriteStatus::invalid_frame || status == rtc::WriteStatus::closed)
          throw std::runtime_error("Publication rejected encoded media");
      }
      timestamp += 960;
      engine->poll();
      while (auto failure = engine->next_failure()) std::cerr << failure->id << ": " << failure->message << '\n';
      std::this_thread::sleep_for(20ms);
    }
    engine->close().get();
    for (const auto& sink : consumers) std::cout << "Received " << sink->frames << " encoded frames, " << sink->bytes << " bytes\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
