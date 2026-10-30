#include "rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "livekit/rtc/video_nodes.hpp"
#include "livekit/rtc/input_node.hpp"
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

template <class Future>
auto await(engine::RtcEngine& engine, Future future) {
  return require_result(std::move(future));
}

class Verify : public graphflow::core::GraphNode {
 public:
  explicit Verify(int expected) : GraphNode("video.verify", nullptr), expected_(expected) {}

  std::atomic<int> frames{0};

 protected:
  bool doInit() override {
    subscribe<nodes::VideoFrameEvent>([this](const auto& value) {
      check(value.pixels && value.pixels->width == 64 && value.pixels->height == 48,
            "Decoded frame size mismatch");
      check(value.epoch && !value.pixels->y.empty(), "Decoded timing/ownership missing");
      for (auto y : value.pixels->y)
        check(std::abs(int(y) - expected_) <= 4, "Video routed to wrong track or corrupted");
      if (last_)
        check(value.timestamp > *last_, "Decoded timestamp regressed");
      last_ = value.timestamp;
      ++frames;
    });
    return true;
  }

 private:
  int expected_;
  std::optional<uint64_t> last_;
};
}  // namespace

int main() {
  try {
    const auto kind = std::getenv("LIVEKIT_MEDIA_ENCRYPTION");
    const bool encrypted = kind && std::string(kind) == "1";
    auto tx = require_result(engine::create_livekit_engine()),
         rx = require_result(engine::create_livekit_engine());
    livekit::sdk::app_support::RtcEventMailbox rx_events(*rx);
    livekit::sdk::app_support::RtcEventMailbox tx_events(*tx);
    LiveKitRoomOptions opts;
    opts.url = std::getenv("LIVEKIT_URL");
    opts.reconnect.enabled = false;
    if (encrypted) {
      auto keys = std::make_shared<livekit::sdk::ExternalKeyProvider>();
      keys->set_passphrase(0, "media-graph-secret");
      opts.encryption.keys = keys;
      opts.encryption.media = true;
    }
    opts.token = std::getenv("LIVEKIT_PUBLISH_TOKEN");
    await(*tx, tx->join(opts));
    opts.token = std::getenv("LIVEKIT_SUBSCRIBE_TOKEN");
    await(*rx, rx->join(opts));
    std::vector<std::shared_ptr<livekit::sdk::I420VideoSource>> sources(2);
    std::vector<std::shared_ptr<nodes::MediaInputNode>> inputs(2);
    std::vector<std::shared_ptr<nodes::VideoCodecNode>> encoders(2), decoders(2);
    std::vector<std::shared_ptr<Verify>> verify(2);
    for (int i = 0; i < 2; ++i) {
      TrackDescriptor d;
      d.id = d.name = "video-" + std::to_string(i);
      d.format.codec = encrypted ? Codec::h264 : Codec::vp8;
      d.format.width = 64;
      d.format.height = 48;
      await(*tx, tx->graph().publish_graph(d, [&, i, d](auto sink) {
        livekit::sdk::I420SourceOptions source_options;
        source_options.width = 64;
        source_options.height = 48;
        sources[i] = std::make_shared<livekit::sdk::I420VideoSource>(source_options);
        nodes::MediaInputOptions input;
        input.video_source = sources[i];
        inputs[i] = std::make_shared<nodes::MediaInputNode>("video.input", d, input);
        encoders[i] = std::make_shared<nodes::VideoCodecNode>("video.encoder", d, true);
        engine::TrackGraphSpec graph;
        graph.nodes = {encoders[i], inputs[i]};
        graph.connections = {
            engine::TrackGraphSpec::connect<nodes::VideoFrameEvent>(inputs[i], encoders[i]),
            engine::TrackGraphSpec::connect<nodes::EncodedFrameEvent>(encoders[i], sink),
            engine::TrackGraphSpec::connect<nodes::SenderFeedbackEvent>(sink, encoders[i]),
            engine::TrackGraphSpec::connect<nodes::SenderFeedbackEvent>(sink, inputs[i])};
        graph.activate = [input = inputs[i]] { input->activate(); };
        graph.quiesce = [input = inputs[i], node = encoders[i]] {
          input->quiesce();
          node->quiesce();
        };
        return graph;
      }));
    }
    const auto discovery_deadline = SteadyClock::now() + 10s;
    while (!verify[0] || !verify[1]) {
      check(SteadyClock::now() < discovery_deadline, "Video graph discovery timed out");
      auto event = rx_events.take_event(100ms);
      if (!event || event->kind != RoomEventKind::remote_track_added)
        continue;
      for (int i = 0; i < 2; ++i)
        if (event->descriptor->name == "video-" + std::to_string(i) && !verify[i]) {
          auto d = *event->descriptor;
          d.id = "receive-" + std::to_string(i);
          await(*rx, rx->graph().subscribe_graph(d, [&, i, d](auto source) {
            decoders[i] = std::make_shared<nodes::VideoCodecNode>("video.decoder", d, false);
            verify[i] = std::make_shared<Verify>(i ? 180 : 60);
            engine::TrackGraphSpec graph;
            graph.nodes = {verify[i], decoders[i]};
            graph.connections = {
                engine::TrackGraphSpec::connect<nodes::EncodedFrameEvent>(source, decoders[i]),
                engine::TrackGraphSpec::connect<nodes::VideoFrameEvent>(decoders[i], verify[i]),
                engine::TrackGraphSpec::connect<nodes::KeyframeRequestEvent>(decoders[i], source)};
            graph.quiesce = [node = decoders[i]] { node->quiesce(); };
            return graph;
          }));
        }
    }
    auto run = [&](int first, int required_first = 10) {
      int baseline = verify[1]->frames;
      const auto deadline = SteadyClock::now() + 12s;
      while (verify[1]->frames < baseline + 10 ||
             (first == 0 && verify[0]->frames < required_first)) {
        check(SteadyClock::now() < deadline, "Raw video graph did not cross SFU");
        for (int i = first; i < 2; ++i) {
          livekit::sdk::VideoFrame frame;
          frame.width = 64;
          frame.height = 48;
          frame.stride_y = 64;
          frame.stride_u = frame.stride_v = 32;
          frame.y.assign(64 * 48, i ? 180 : 60);
          frame.u.assign(32 * 24, 96);
          frame.v.assign(32 * 24, 160);
          check(sources[i]->write(frame) == livekit::sdk::VideoWriteStatus::accepted,
                "Application source rejected live pixels");
        }

        if (auto fail = tx_events.take_failure())
          throw std::runtime_error(fail->message);
        if (auto fail = rx_events.take_failure())
          throw std::runtime_error(fail->message);
        std::this_thread::sleep_for(33ms);
      }
    };
    run(0);
    await(*tx, tx->set_muted("video-0", true));

    const auto muted_admissions = encoders[0]->snapshot().admitted;
    run(0);  // Keep producing both sources; only the other graph may encode.
    check(encoders[0]->snapshot().admitted == muted_admissions,
          "Muted engine graph continued encoding raw input");
    await(*tx, tx->set_muted("video-0", false));
    run(0, verify[0]->frames.load() + 10);
    await(*tx, tx->unpublish("video-0"));
    await(*rx, rx->unsubscribe("receive-0"));
    run(1);
    require_result(tx->close());
    require_result(rx->close());
    for (auto source : sources)
      check(!source->stats().attached, "Input source lease survived graph close");
    for (auto node : inputs)
      node->throw_if_failed();
    for (auto node : encoders) {
      node->throw_if_failed();
      check(node->snapshot().queued_frames == 0, "Encoder queue survived close");
    }
    for (auto node : decoders) {
      node->throw_if_failed();
      check(node->snapshot().queued_frames == 0, "Decoder queue survived close");
    }
    std::cout << "{\"rawVideoGraphs\":2,\"inputNodes\":true,\"independentClose\":true,"
                 "\"muteRecovery\":true,\"codecOutsideTransport\":true,\"encrypted\":"
              << (encrypted ? "true" : "false") << "}\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
