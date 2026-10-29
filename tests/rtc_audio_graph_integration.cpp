#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "rtc_audio_fixtures.hpp"
#include "media_nodes/playout/audio_output_driver.hpp"
#include <array>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

namespace {
void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

template <class T>
auto await(engine::RtcEngine& engine, std::future<T> future) {
  const auto deadline = SteadyClock::now() + 15s;
  while (future.wait_for(0ms) != std::future_status::ready) {
    check(SteadyClock::now() < deadline, "Audio graph operation timed out");

    std::this_thread::sleep_for(1ms);
  }
  return future.get();
}

class Source : public graphflow::core::GraphNode {
 public:
  Source() : GraphNode("pcm.input", nullptr) {}

  void send(nodes::AudioFrameEvent frame) {
    publish(frame);
  }
};

// Device clock driven by the test loop. No physical playback during automation.
class TestOutputDevice final : public detail::AudioOutputDriver {
 public:
  std::mutex mutex;
  Render callback;
  int starts = 0, stops = 0;

  void start(const AudioOutputOptions&, Render render, Failure) override {
    std::lock_guard lock(mutex);
    callback = std::move(render);
    ++starts;
  }

  void stop() noexcept override {
    std::lock_guard lock(mutex);
    if (callback)
      ++stops;
    callback = {};
  }

  std::array<int16_t, 960> pull() {
    std::lock_guard lock(mutex);
    std::array<int16_t, 960> result{};
    if (callback)
      callback(result);
    return result;
  }
};

class Verify : public graphflow::core::GraphNode {
 public:
  explicit Verify(int index) : GraphNode("pcm.verify", nullptr), index_(index) {}

  std::atomic<int> frames{0};

 protected:
  bool doInit() override {
    subscribe<nodes::AudioFrameEvent>([this](const auto& event) {
      check(event.pcm && event.pcm->sample_rate == 48000 && event.pcm->channels == 2 &&
                event.pcm->samples_per_channel() == 960 && event.epoch,
            "Decoded audio format mismatch");
      if (last_) {
        check(event.timestamp > *last_, "Decoded audio clock regressed");
        if (!event.discontinuity)
          check(event.timestamp == *last_ + 960, "Decoded audio gap was hidden");
      }
      last_ = event.timestamp;
      if (frames > 3)
        for (size_t ch = 0; ch < 2; ++ch) {
          const double frequency = index_ ? 880 * (1 + .5 * ch) : 440;
          const auto intended = rtc_audio_test::tone_power(*event.pcm, ch, frequency);
          const auto other = rtc_audio_test::tone_power(*event.pcm, ch, index_ ? 440 : 880);
          check(intended > 1000000 && intended > other * 5,
                "Audio content or stream/channel routing mismatch");
        }
      ++frames;
    });
    return true;
  }

 private:
  int index_;
  std::optional<uint64_t> last_;
};
}  // namespace

int main() {
  try {
    const auto encrypted_env = std::getenv("LIVEKIT_MEDIA_ENCRYPTION");
    const bool encrypted = encrypted_env && std::string(encrypted_env) == "1";
    auto tx = engine::create_livekit_engine(), rx = engine::create_livekit_engine();
    livekit::sdk::app_support::RtcEventMailbox rx_events(*rx);
    livekit::sdk::app_support::RtcEventMailbox tx_events(*tx);
    LiveKitRoomOptions options;
    options.url = std::getenv("LIVEKIT_URL");
    options.reconnect.enabled = false;
    if (encrypted) {
      auto keys = std::make_shared<livekit::sdk::ExternalKeyProvider>();
      keys->set_passphrase(0, "audio-graph-secret");
      options.encryption.keys = keys;
      options.encryption.media = true;
    }
    options.token = std::getenv("LIVEKIT_PUBLISH_TOKEN");
    await(*tx, tx->join(options));
    options.token = std::getenv("LIVEKIT_SUBSCRIBE_TOKEN");
    await(*rx, rx->join(options));
    std::vector<std::shared_ptr<Source>> sources(2);
    std::vector<std::shared_ptr<nodes::AudioCodecNode>> encoders(2), decoders(2);
    std::vector<std::shared_ptr<Verify>> outputs(2);
    auto output_driver = std::make_unique<TestOutputDevice>();
    auto* device = output_driver.get();
    auto playout = detail::AudioOutputFactory::create({}, std::move(output_driver));
    std::vector<std::shared_ptr<nodes::AudioOutputNode>> speakers(2);
    using G = engine::TrackGraphSpec;
    for (int i = 0; i < 2; ++i) {
      const auto d = rtc_audio_test::descriptor("audio-" + std::to_string(i), i + 1);
      await(*tx, tx->graph().publish_graph(d, [&, i, d](auto sink) {
        sources[i] = std::make_shared<Source>();
        encoders[i] = std::make_shared<nodes::AudioCodecNode>("opus.encoder", d, true);
        G graph;
        graph.nodes = {encoders[i], sources[i]};
        graph.connections = {G::connect<nodes::AudioFrameEvent>(sources[i], encoders[i]),
                             G::connect<nodes::EncodedFrameEvent>(encoders[i], sink),
                             G::connect<nodes::SenderFeedbackEvent>(sink, encoders[i])};
        graph.quiesce = [node = encoders[i]] { node->quiesce(); };
        graph.check_health = [node = encoders[i]] { node->throw_if_failed(); };
        return graph;
      }));
    }
    const auto deadline = SteadyClock::now() + 10s;
    while (!outputs[0] || !outputs[1]) {
      check(SteadyClock::now() < deadline, "Audio graph discovery timed out");
      auto event = rx_events.take_event(100ms);
      if (!event || event->kind != RoomEventKind::remote_track_added)
        continue;
      for (int i = 0; i < 2; ++i)
        if (event->descriptor->name == "audio-" + std::to_string(i) && !outputs[i]) {
          auto d = *event->descriptor;
          d.id = "receive-" + std::to_string(i);
          await(*rx, rx->graph().subscribe_graph(d, [&, i, d](auto source) {
            decoders[i] = std::make_shared<nodes::AudioCodecNode>("opus.decoder", d, false);
            outputs[i] = std::make_shared<Verify>(i);
            speakers[i] = std::make_shared<nodes::AudioOutputNode>("device.output", d.id, playout);
            G graph;
            graph.nodes = {speakers[i], outputs[i], decoders[i]};
            graph.connections = {G::connect<nodes::EncodedFrameEvent>(source, decoders[i]),
                                 G::connect<nodes::AudioFrameEvent>(decoders[i], outputs[i]),
                                 G::connect<nodes::AudioFrameEvent>(decoders[i], speakers[i])};
            graph.quiesce = [node = decoders[i], output = speakers[i]] {
              node->quiesce();
              output->quiesce();
            };
            graph.check_health = [node = decoders[i], output = speakers[i]] {
              node->throw_if_failed();
              output->throw_if_failed();
            };
            return graph;
          }));
        }
    }
    uint64_t timestamp = 0xfffffe20ULL;
    const auto run = [&](int first) {
      const auto baseline = outputs[1]->frames.load();
      PcmAudioFrame rendered;
      rendered.channels = 2;
      const auto end = SteadyClock::now() + 12s;
      while (outputs[1]->frames < baseline + 15 || (first == 0 && outputs[0]->frames < 15)) {
        check(SteadyClock::now() < end, "Audio graphs failed to send PCM through SFU");
        for (int i = first; i < 2; ++i)
          sources[i]->send(rtc_audio_test::tone(tx->publication("audio-" + std::to_string(i)).epoch,
                                                timestamp, i ? 48000 : 44100, i + 1, !i,
                                                i ? 880 : 440));
        timestamp += 480;

        if (auto failure = tx_events.take_failure())
          throw std::runtime_error(failure->message);
        if (auto failure = rx_events.take_failure())
          throw std::runtime_error(failure->message);
        const auto block = device->pull();
        rendered.samples.insert(rendered.samples.end(), block.begin(), block.end());
        std::this_thread::sleep_for(10ms);
      }
      const auto low = rtc_audio_test::tone_power(rendered, 0, 440),
                 high = rtc_audio_test::tone_power(rendered, 0, 880);
      check(high > 100000, "Shared output did not render second track's tone");
      if (first == 0)
        check(low > 100000, "Shared output did not mix first track's tone");
      else
        check(high > low * 5, "Closed output track continued contributing to mix");
    };
    run(0);
    await(*tx, tx->unpublish("audio-0"));
    await(*rx, rx->unsubscribe("receive-0"));
    check(device->starts == 1 && device->stops == 0 && playout->snapshot().tracks == 1,
          "Independent subscription close stopped shared device");
    run(1);
    tx->close().get();
    rx->close().get();
    check(device->stops == 1 && playout->snapshot().tracks == 0,
          "Final subscription close retained device");
    for (auto node : encoders) {
      node->throw_if_failed();
      check(node->snapshot().queued_frames == 0, "PCM queue survived close");
    }
    for (auto node : decoders) {
      node->throw_if_failed();
      check(node->snapshot().queued_frames == 0, "Opus queue survived close");
    }
    std::cout << "{\"rawAudioGraphs\":2,\"resampled44100\":true,\"stereo\":true,\"sharedOutput\":"
                 "true,\"independentClose\":true,\"encrypted\":"
              << (encrypted ? "true" : "false") << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
