#include "service_test_result.hpp"
#include "engines/player/playback_graph.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <tuple>

using namespace livekit::sdk;
using namespace livekit::sdk::detail;

namespace {
void require(bool ok, const char* message) {
  if (!ok) {
    std::cerr << message << "\n";
    std::abort();
  }
}

template <class F>
void fails(F operation) {
  require(!operation(), "expected operation to fail");
}

void state_is(const PlaybackGraph& graph, MediaState expected) {
  for (const auto& node : graph.nodes()) {
    require(node.state == expected, "node lifecycle mismatch");
  }
}

// open() negotiates both branches, including a description for an absent stream.
// It must not run the packet/frame pipeline just to configure the decoders.
void check_open_delivery(const PlaybackGraph& graph) {
  state_is(graph, MediaState::Initialized);
  require(graph.outputs()->empty(), "open produced output frames");
  std::set<std::string> receivers;
  for (const auto& node : graph.nodes()) {
    if (node.name == "file.demuxer") {
      require(node.received == 0 && node.published == 2, "expected two stream descriptions");
    } else if (node.name == "video.decoder" || node.name == "audio.decoder") {
      require(node.received == 1 && node.published == 0, "decoder did not accept just its format");
      receivers.insert(node.name);
    } else {
      require(node.received == 0 && node.published == 0, "media reached downstream before start");
    }
  }
  require(receivers.size() == 2, "missing decoder branch during open");
}

int drain(PlaybackGraph& graph, uint64_t generation, int64_t lower_bound) {
  auto output = graph.outputs();
  int video = 0, audio = 0;
  for (int i = 0; i < 10000; ++i) {
    auto step = service_value(graph.step());
    while (auto frame = output->video->take_due(std::numeric_limits<int64_t>::max(), generation)) {
      require(frame->generation == generation && frame->timestamp_us >= lower_bound,
              "stale video after graph seek");
      ++video;
    }
    while (auto frame = output->audio->take_due(std::numeric_limits<int64_t>::max(), generation)) {
      require(frame->generation == generation && frame->timestamp_us >= lower_bound,
              "stale audio after graph seek");
      ++audio;
    }
    if (step == PlaybackGraph::Step::end) {
      require(output->empty() && audio > 0, "output/decoder EOS did not drain");
      return video;
    }
  }
  require(false, "graph did not finish");
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  require(argc == 2, "fixture directory required");
  const std::filesystem::path fixtures = argv[1];
  auto file = [&](const char* name) {
    auto path = (fixtures / name).u8string();
    return std::string(path.begin(), path.end());
  };
  std::atomic<bool> cancelled{false};
  PlayerOptions options;
  options.video_queue_frames = 2;
  options.audio_queue_frames = 3;
  std::optional<media::VideoFrame> retained;
  std::shared_ptr<PlaybackOutputs> released_outputs;
  {
    PlaybackGraph graph(cancelled, options);
    require(graph.nodes().size() == 7 && graph.edges().size() == 6,
            "expected seven real nodes and six media connections");
    std::set<std::string> names;
    for (const auto& node : graph.nodes()) {
      names.insert(node.name);
    }
    require(names == std::set<std::string>{"file.demuxer", "video.decoder", "audio.decoder",
                                           "video.rgba", "audio.resampler", "video.output",
                                           "audio.output"},
            "node identities mismatch");
    for (const auto& edge : graph.edges()) {
      require(names.count(edge.from) && names.count(edge.to), "dangling graph edge");
      require(!edge.type.empty() && !edge.port.empty(),
              "media edge must carry typed pub/sub events");
      if (edge.from == "file.demuxer") {
        require(edge.port == (edge.to == "video.decoder" ? "video" : "audio"),
                "demux output route mismatch");
      }
    }
    using Edge = std::tuple<std::string, std::string, std::string>;
    std::set<Edge> actual_edges;
    for (const auto& edge : graph.edges()) {
      actual_edges.emplace(edge.from, edge.to, edge.type);
    }
    require(actual_edges == std::set<Edge>{{"file.demuxer", "video.decoder", "VideoPacket"},
                                           {"file.demuxer", "audio.decoder", "AudioPacket"},
                                           {"video.decoder", "video.rgba", "DecodedVideo"},
                                           {"audio.decoder", "audio.resampler", "DecodedAudio"},
                                           {"video.rgba", "video.output", "RGBA8"},
                                           {"audio.resampler", "audio.output", "StereoFloat48k"}},
            "builder connected the wrong playback branches");
    state_is(graph, MediaState::Created);
    fails([&] { return graph.start(); });
    fails([&] { return graph.seek(0); });
    state_is(graph, MediaState::Created);
    auto info = service_value(graph.open(file("av.mp4")));
    require(info.has_video && info.has_audio, "demux probe");
    check_open_delivery(graph);
    fails([&] { return graph.step(); });
    auto output = graph.outputs();
    output->flush(10);
    service_value(graph.start());
    state_is(graph, MediaState::Running);
    bool blocked = false;
    for (int i = 0; i < 1000; ++i) {
      if (service_value(graph.step()) == PlaybackGraph::Step::backpressured) {
        blocked = true;
        break;
      }
    }
    require(blocked && output->video->size() <= 2 && output->audio->size() <= 3,
            "output nodes failed to backpressure");
    retained = output->video->take_due(std::numeric_limits<int64_t>::max(), 10);
    require(retained && !retained->rgba->empty(), "missing retained video buffer");
    service_value(graph.pause());
    state_is(graph, MediaState::Stopped);
    fails([&] { return graph.step(); });
    service_value(graph.seek(1800000));
    output->flush(11);
    require(output->empty(), "seek left old output frames");
    service_value(graph.start());
    require(drain(graph, 11, 1800000) == 15, "seek lost delayed decoder frames");
    require(graph.end_us() >= 2400000, "graph ended before audio/video tails");
    service_value(graph.pause());
    service_value(graph.seek(0));
    output->flush(12);
    service_value(graph.start());
    require(drain(graph, 12, 0) == 60, "flush/replay lost frames");
    const auto diagnostics = graph.nodes();
    for (const auto& node : diagnostics) {
      if (node.name == "file.demuxer") {
        require(node.received == 0, "file demuxer must be the graph source");
      } else {
        require(node.received > 0, "node did not receive media events");
      }
      if (node.name != "video.output" && node.name != "audio.output") {
        require(node.published > 0, "node bypassed publication");
      }
    }
    released_outputs = output;
  }
  require(retained->rgba->size() == 160 * 90 * 4, "buffer invalidated on graph release");
  require(released_outputs->empty() && released_outputs->video->state() == MediaState::Released,
          "output nodes not released with graph");
  fails([&] {
    auto copy = *retained;
    return released_outputs->video->push(copy);
  });
  for (bool video_only : {false, true}) {
    PlaybackGraph graph(cancelled, options);
    auto info = service_value(graph.open(file(video_only ? "video.mp4" : "audio.wav")));
    require(info.has_video == video_only && info.has_audio == !video_only, "single-stream probe");
    check_open_delivery(graph);
  }
  // Valid mono WAVE container, deliberately unassigned codec tag. Probing succeeds;
  // the audio decoder's rejection must reach open(), not a later start()/step().
  {
    const unsigned char wave[] = {'R',  'I',  'F', 'F', 40,   0,    0, 0, 'W',  'A',  'V', 'E',
                                  'f',  'm',  't', ' ', 16,   0,    0, 0, 0xfe, 0xca, 1,   0,
                                  0x40, 0x1f, 0,   0,   0x80, 0x3e, 0, 0, 2,    0,    16,  0,
                                  'd',  'a',  't', 'a', 4,    0,    0, 0, 0,    0,    0,   0};
    std::ofstream fixture(fixtures / "unsupported-codec.wav", std::ios::binary);
    fixture.write(reinterpret_cast<const char*>(wave), sizeof(wave));
    fixture.close();
    require(bool(fixture), "could not write unsupported codec fixture");
    PlaybackGraph graph(cancelled, options);
    auto result = graph.open(file("unsupported-codec.wav"));
    require(!result && result.error().message == "playback/audio.decoder: unsupported codec",
            "open lost the decoder's setup failure");
    require(graph.outputs()->empty(), "failed open produced media");
    state_is(graph, MediaState::Released);
    fails([&] { return graph.start(); });
  }
  // Demux failure happens after opening the local file. Roll back all nodes.
  {
    PlaybackGraph graph(cancelled, options);
    fails([&] { return graph.open(file("invalid.mp4")); });
    state_is(graph, MediaState::Released);
    fails([&] { return graph.start(); });
    fails([&] { return graph.step(); });
  }
  // Initialization failure precedes media preparation; the engine still owns
  // and releases every node (including subscribers initialized downstream).
  {
    PlaybackGraph graph(cancelled, options);
    auto missing = graph.open(file("missing-file.mp4"));
    require(!missing && missing.error().message ==
                            "playback/file.demuxer/io: expected a local regular file",
            "open lost the file node init failure");
    fails([&] { return graph.seek(0); });
    state_is(graph, MediaState::Released);
  }
  {
    PlaybackGraph graph(cancelled, options);
    service_value(graph.open(file("av.mp4")));
    service_value(graph.start());
    cancelled = true;
    fails([&] { return graph.step(); });
    cancelled = false;
  }
  std::cout << "PASS: pub/sub topology and delivery, node lifecycle, output backpressure, flush "
               "generations, EOS, rollback and cancellation\n";
  return 0;
}
