#include "engines/player/playback_graph.hpp"
#include "backends/ffmpeg/ffmpeg_playback.hpp"
#include "engine/graph_engine.h"
#include <algorithm>

namespace livekit::sdk::detail {
class PlaybackGraph::Impl {
 public:
  Impl(std::atomic<bool>& cancel, const PlayerOptions& options)
      : cancelled(cancel), source(std::make_shared<FileSourceNode>(engine.eventBus(), cancel, options.io_timeout)),
        demux(std::make_shared<DemuxerNode>(engine.eventBus(), cancel, options.io_timeout)),
        video_decoder(std::make_shared<VideoDecoderNode>(engine.eventBus())),
        audio_decoder(std::make_shared<AudioDecoderNode>(engine.eventBus())),
        video_converter(std::make_shared<VideoConverterNode>(engine.eventBus())),
        audio_resampler(std::make_shared<AudioResamplerNode>(engine.eventBus())),
        output(std::make_shared<PlaybackOutputs>(engine.eventBus(), options)) {
    nodes = {source, demux, video_decoder, audio_decoder, video_converter, audio_resampler, output->video, output->audio};
    // GraphEngine preserves the supplied lifecycle order. Install downstream
    // subscriptions first, and release the source only after all consumers.
    std::vector<std::shared_ptr<graphflow::core::GraphNode>> lifecycle_nodes(nodes.rbegin(), nodes.rend());
    const std::vector<graphflow::engine::NodeConnection> connections = {
        {"file.source", "file.demuxer", "SeekableBytes", ""},
        {"file.demuxer", "video.decoder", "VideoPacket", "video"},
        {"file.demuxer", "audio.decoder", "AudioPacket", "audio"},
        {"video.decoder", "video.rgba", "DecodedVideo", ""},
        {"audio.decoder", "audio.resampler", "DecodedAudio", ""},
        {"video.rgba", "video.output", "RGBA8", ""},
        {"audio.resampler", "audio.output", "StereoFloat48k", ""},
    };
    if (!engine.loadFromNodes(lifecycle_nodes, connections)) throw PlayerError("playback/build: graph load failed");
    for (const auto& node : nodes) {
      const auto& outgoing = node->getPubEdges();
      edges.insert(edges.end(), outgoing.begin(), outgoing.end());
    }
    executor = std::make_unique<MediaExecutor>(nodes);
    nodes = executor->nodes();
    engine.eventBus()->subscribe<Control>("player.control", [this](Control& event) {
      try {
        if (event.kind == Control::start) {
          if (!engine.start()) throw PlayerError("playback/start: graph is not prepared");
        } else if (event.kind == Control::pause) engine.stop();
        else flush_seek(event.position);
      } catch (...) { event.error = std::current_exception(); }
    });
  }
  ~Impl() { engine.release(); }
  struct Control : graphflow::core::EventT<Control> {
    enum Kind { start, pause, seek } kind;
    int64_t position = 0;
    std::exception_ptr error;
    Control(Kind kind, int64_t position = 0) : kind(kind), position(position) {}
  };
  void dispatch(Control event) {
    engine.eventBus()->publish("player.control", event);
    if (event.error) std::rethrow_exception(event.error);
  }
  PlayerMediaInfo open(const std::string& filename) {
    if (engine.state() != graphflow::core::LifecycleState::Created) throw std::logic_error("playback/open: graph already used");
    source->configure(filename);
    try {
      if (!engine.init()) throw PlayerError("playback/prepare: graph init failed");
      // Media preparation may publish stream formats after every subscription
      // exists. GraphEngine alone drives init/start/stop/release.
      for (const auto& node : nodes) node->prepare();
    } catch (...) { engine.release(); edges.clear(); throw; }
    return demux->info();
  }
  void flush_seek(int64_t position) {
    if (position < 0) throw std::invalid_argument("playback/seek: negative position");
    executor->reset(position);
  }
  Step step() {
    if (engine.state() != graphflow::core::LifecycleState::Running) throw std::logic_error("playback/step: graph is not running");
    if (cancelled.load()) throw PlayerError("playback: cancelled");
    switch (executor->step()) {
      case MediaExecutor::Step::progress: return Step::progress;
      case MediaExecutor::Step::backpressured: return Step::backpressured;
      case MediaExecutor::Step::end: return Step::end;
    }
    throw std::logic_error("playback: invalid executor result");
  }
  graphflow::engine::GraphEngine engine{1};
  std::atomic<bool>& cancelled;
  std::shared_ptr<FileSourceNode> source;
  std::shared_ptr<DemuxerNode> demux;
  std::shared_ptr<VideoDecoderNode> video_decoder;
  std::shared_ptr<AudioDecoderNode> audio_decoder;
  std::shared_ptr<VideoConverterNode> video_converter;
  std::shared_ptr<AudioResamplerNode> audio_resampler;
  std::shared_ptr<PlaybackOutputs> output;
  std::vector<std::shared_ptr<MediaNode>> nodes;
  std::vector<graphflow::core::GraphEdge> edges;
  std::unique_ptr<MediaExecutor> executor;
};
PlaybackGraph::PlaybackGraph(std::atomic<bool>& cancel, const PlayerOptions& options)
    : impl_(std::make_unique<Impl>(cancel, options)) {}
PlaybackGraph::~PlaybackGraph() = default;
PlayerMediaInfo PlaybackGraph::open(const std::string& path) { return impl_->open(path); }
PlaybackGraph::Step PlaybackGraph::step() { return impl_->step(); }
int64_t PlaybackGraph::end_us() const { return std::max(impl_->output->video->end_us(), impl_->output->audio->end_us()); }
std::shared_ptr<PlaybackOutputs> PlaybackGraph::outputs() const { return impl_->output; }
std::vector<PlaybackGraph::NodeInfo> PlaybackGraph::nodes() const {
  std::vector<NodeInfo> result;
  for (const auto& node : impl_->nodes) result.push_back({node->getName(), node->state(), node->received(), node->published()});
  return result;
}
std::vector<graphflow::core::GraphEdge> PlaybackGraph::edges() const { return impl_->edges; }
void PlaybackGraph::seek(int64_t position) { impl_->dispatch(Impl::Control(Impl::Control::seek, position)); }
void PlaybackGraph::start() { impl_->dispatch(Impl::Control(Impl::Control::start)); }
void PlaybackGraph::pause() { impl_->dispatch(Impl::Control(Impl::Control::pause)); }
}
