#include "engines/player/playback_graph.hpp"
#include "engine/graph_builder.h"
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <tuple>

using namespace livekit::sdk;
using namespace livekit::sdk::detail;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void fails(F operation) {
  bool failed = false;
  try { operation(); } catch (const std::exception&) { failed = true; }
  require(failed, "operation should have failed");
}
void state_is(const PlaybackGraph& graph, graphflow::core::LifecycleState expected) {
  for (const auto& node : graph.nodes()) require(node.state == expected, "node lifecycle mismatch");
}
void check_builder_transports() {
  using namespace graphflow;
  core::EventBus bus{1};
  engine::GraphBuilder builder;
  builder.setEventBus(&bus);
  auto a = std::make_shared<core::GraphNode>("a", &bus);
  auto b = std::make_shared<core::GraphNode>("b", &bus);
  auto c = std::make_shared<core::GraphNode>("c", &bus);
  require(builder.registerNode("a", a) && builder.registerNode("b", b) && builder.registerNode("c", c), "builder registration");
  using Specs = std::vector<engine::PathNodeSpec>;
  constexpr auto external = engine::GraphBuilder::EdgeTransport::External;
  fails([&] { builder.build(Specs{{"a", "bytes", ""}, {"b", "packet", ""}, {"missing", "", ""}}, external); });
  require(a->getPubEdges().empty() && b->getSubEdges().empty(), "invalid path partially attached edges");
  auto input = builder.build(Specs{{"a", "bytes", ""}, {"b", "", ""}}, external);
  auto branch = builder.build(Specs{{"a", "bytes", ""}, {"b", "packet", "video"}, {"c", "", ""}}, external);
  require(input->eventBusesSnapshot().empty() && branch->eventBusesSnapshot().empty(), "external edges allocated buses");
  require(a->getPubEdges().size() == 1 && b->getSubEdges().size() == 1 && b->getPubEdges().size() == 1,
      "builder did not deduplicate shared external edge");
  require(!b->getPubEdges().front().eventBus() && b->getPubEdges().front().condition() == "video", "external routing metadata");
  fails([&] { builder.build(Specs{{"a", "bytes", ""}, {"b", "", ""}}); });
  auto control = builder.build(Specs{{"a", "command", ""}, {"c", "", ""}});
  require(control->eventBusesSnapshot().size() == 1 && a->getPubEdges().back().eventBus(), "default EventBus transport changed");
  fails([&] { builder.build(Specs{{"a", "command", ""}, {"c", "", ""}}, external); });
  builder.reset();
  require(!control->eventBusesSnapshot().front()->isClosed(), "builder reset invalidated path bus");
  require(a->state() == core::LifecycleState::Created, "builder released nodes during construction");
}
int drain(PlaybackGraph& graph, uint64_t generation, int64_t lower_bound) {
  auto output = graph.outputs();
  int video = 0, audio = 0;
  for (int i = 0; i < 10000; ++i) {
    auto step = graph.step();
    while (auto frame = output->video->poll(std::numeric_limits<int64_t>::max(), generation)) {
      require(frame->generation == generation && frame->timestamp_us >= lower_bound, "stale video after graph seek"); ++video;
    }
    while (auto frame = output->audio->poll(std::numeric_limits<int64_t>::max(), generation)) {
      require(frame->generation == generation && frame->timestamp_us >= lower_bound, "stale audio after graph seek"); ++audio;
    }
    if (step == PlaybackGraph::Step::end) {
      require(output->empty() && audio > 0, "output/decoder EOS did not drain"); return video;
    }
  }
  throw std::runtime_error("graph did not finish");
}
}
int main(int argc, char** argv) {
  try {
    require(argc == 2, "fixture directory required");
    check_builder_transports();
    const std::filesystem::path fixtures = argv[1];
    auto file = [&](const char* name) { auto path = (fixtures / name).u8string(); return std::string(path.begin(), path.end()); };
    std::atomic<bool> cancelled{false};
    PlayerOptions options; options.video_queue_frames = 2; options.audio_queue_frames = 3;
    std::optional<media::VideoFrame> retained;
    std::shared_ptr<PlaybackOutputs> released_outputs;
    {
      PlaybackGraph graph(cancelled, options);
      require(graph.nodes().size() == 8 && graph.edges().size() == 7, "expected eight real nodes and seven media connections");
      std::set<std::string> names;
      for (const auto& node : graph.nodes()) names.insert(node.name);
      require(names == std::set<std::string>{"file.source", "file.demuxer", "video.decoder", "audio.decoder",
          "video.rgba", "audio.resampler", "video.output", "audio.output"}, "node identities mismatch");
      for (const auto& edge : graph.edges()) {
        require(names.contains(edge.from()) && names.contains(edge.to()), "dangling graph edge");
        require(!edge.dataType().empty() && edge.eventBus(), "media edge must carry typed pub/sub events");
        if (edge.from() == "file.demuxer") require(edge.condition() == (edge.to() == "video.decoder" ? "video" : "audio"),
            "demux output route mismatch");
      }
      using Edge = std::tuple<std::string, std::string, std::string>;
      std::set<Edge> actual_edges;
      for (const auto& edge : graph.edges()) actual_edges.emplace(edge.from(), edge.to(), edge.dataType());
      require(actual_edges == std::set<Edge>{
          {"file.source", "file.demuxer", "SeekableBytes"},
          {"file.demuxer", "video.decoder", "VideoPacket"}, {"file.demuxer", "audio.decoder", "AudioPacket"},
          {"video.decoder", "video.rgba", "DecodedVideo"}, {"audio.decoder", "audio.resampler", "DecodedAudio"},
          {"video.rgba", "video.output", "RGBA8"}, {"audio.resampler", "audio.output", "StereoFloat48k"}},
          "builder connected the wrong playback branches");
      state_is(graph, graphflow::core::LifecycleState::Created);
      fails([&] { graph.start(); });
      state_is(graph, graphflow::core::LifecycleState::Created);
      auto info = graph.open(file("av.mp4"));
      require(info.has_video && info.has_audio, "demux probe");
      for (const auto& edge : graph.edges()) require(edge.eventBus()->handlerCount(edge.name()) == 1, "missing media subscriber");
      state_is(graph, graphflow::core::LifecycleState::Initialized);
      fails([&] { graph.step(); });
      auto output = graph.outputs(); output->flush(10);
      graph.start(); state_is(graph, graphflow::core::LifecycleState::Running);
      bool blocked = false;
      for (int i = 0; i < 1000; ++i) if (graph.step() == PlaybackGraph::Step::backpressured) { blocked = true; break; }
      require(blocked && output->video->size() <= 2 && output->audio->size() <= 3, "output nodes failed to backpressure");
      retained = output->video->poll(std::numeric_limits<int64_t>::max(), 10);
      require(retained && !retained->rgba->empty(), "missing retained video buffer");
      graph.pause(); state_is(graph, graphflow::core::LifecycleState::Stopped);
      fails([&] { graph.step(); });
      graph.seek(1800000); output->flush(11);
      require(output->empty(), "seek left old output frames");
      graph.start();
      require(drain(graph, 11, 1800000) == 15, "seek lost delayed decoder frames");
      require(graph.end_us() >= 2400000, "graph ended before audio/video tails");
      graph.pause(); graph.seek(0); output->flush(12); graph.start();
      require(drain(graph, 12, 0) == 60, "flush/replay lost frames");
      const auto diagnostics = graph.nodes();
      for (const auto& node : diagnostics) {
        if (node.name == "file.source") require(node.published > 0, "source did not publish file bytes");
        else require(node.received > 0, "node did not receive media events");
        if (node.name != "video.output" && node.name != "audio.output") require(node.published > 0, "node bypassed publication");
      }
      released_outputs = output;
    }
    require(retained->rgba->size() == 160 * 90 * 4, "buffer invalidated on graph release");
    require(released_outputs->empty() && released_outputs->video->state() == graphflow::core::LifecycleState::Released,
        "output nodes not released with graph");
    fails([&] { auto copy = *retained; released_outputs->video->push(copy); });
    // Demux failure happens after preparing the file source. Roll back all nodes.
    {
      PlaybackGraph graph(cancelled, options);
      fails([&] { graph.open(file("invalid.mp4")); });
      state_is(graph, graphflow::core::LifecycleState::Released);
      fails([&] { graph.start(); });
      fails([&] { graph.step(); });
    }
    // Initialization failure precedes media preparation; the engine still owns
    // and releases every node (including subscribers initialized downstream).
    {
      PlaybackGraph graph(cancelled, options);
      fails([&] { graph.open(file("missing-file.mp4")); });
      state_is(graph, graphflow::core::LifecycleState::Released);
    }
    {
      PlaybackGraph graph(cancelled, options);
      graph.open(file("av.mp4")); graph.start(); cancelled = true;
      fails([&] { graph.step(); });
      cancelled = false;
    }
    std::cout << "PASS: pub/sub topology and delivery, node lifecycle, output backpressure, flush generations, EOS, rollback and cancellation\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
