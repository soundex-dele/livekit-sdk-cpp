#include "engines/player/playback_graph.hpp"
#include "backends/ffmpeg/ffmpeg_playback.hpp"
#include "engine/graph_engine.h"
#include "media_graph/failure.hpp"
#include <algorithm>

namespace livekit::sdk::detail {
class PlaybackGraph::Impl {
 public:
  Impl(std::atomic<bool>& cancel, const PlayerOptions& options)
      : cancelled(cancel),
        source(std::make_shared<FileSourceNode>(engine.eventBus(), cancel, options.io_timeout)),
        demux(std::make_shared<DemuxerNode>(engine.eventBus(), cancel, options.io_timeout)),
        video_decoder(std::make_shared<VideoDecoderNode>(engine.eventBus())),
        audio_decoder(std::make_shared<AudioDecoderNode>(engine.eventBus())),
        video_converter(std::make_shared<VideoConverterNode>(engine.eventBus())),
        audio_resampler(std::make_shared<AudioResamplerNode>(engine.eventBus())),
        output(std::make_shared<PlaybackOutputs>(engine.eventBus(), options)) {
    nodes = {source,          demux,           video_decoder, audio_decoder,
             video_converter, audio_resampler, output->video, output->audio};
    // GraphEngine preserves the supplied lifecycle order. Install downstream
    // subscriptions first, and release the source only after all consumers.
    std::vector<std::shared_ptr<graphflow::core::GraphNode>> lifecycle_nodes(nodes.rbegin(),
                                                                             nodes.rend());
    const std::vector<graphflow::engine::NodeConnection> connections = {
        {"file.source", "file.demuxer", "SeekableBytes", ""},
        {"file.demuxer", "video.decoder", "VideoPacket", "video"},
        {"file.demuxer", "audio.decoder", "AudioPacket", "audio"},
        {"video.decoder", "video.rgba", "DecodedVideo", ""},
        {"audio.decoder", "audio.resampler", "DecodedAudio", ""},
        {"video.rgba", "video.output", "RGBA8", ""},
        {"audio.resampler", "audio.output", "StereoFloat48k", ""},
    };
    if (!engine.loadFromNodes(lifecycle_nodes, connections)) {
      build_failure = make_failure(ErrorDomain::media, ErrorCode::processing_failed,
                                   "playback/build: graph load failed");
      return;
    }
    for (const auto& node : nodes) {
      const auto& outgoing = node->getPubEdges();
      edges.insert(edges.end(), outgoing.begin(), outgoing.end());
    }
    executor = std::make_unique<MediaExecutor>(nodes);
    nodes = executor->nodes();
    engine.eventBus()->subscribe<Control>("player.control", [this](Control& event) {
      if (event.kind == Control::start) {
        if (!engine.start()) {
          event.error = node_failure("playback/start: graph is not prepared");
        }
      } else if (event.kind == Control::pause) {
        engine.stop();
      } else {
        auto result = flush_seek(event.position);
        if (!result) {
          event.error = result.failure_record();
        }
      }
    });
  }

  ~Impl() {
    engine.release();
  }

  struct Control : graphflow::core::EventT<Control> {
    enum Kind { start, pause, seek } kind;

    int64_t position = 0;
    Failure error;

    Control(Kind kind, int64_t position = 0) : kind(kind), position(position) {}
  };

  Failure node_failure(const std::string& fallback) const {
    for (const auto& node : nodes) {
      if (auto error = node->error()) {
        return graph_failure(*error);
      }
    }
    return make_failure(ErrorDomain::media, ErrorCode::processing_failed, fallback);
  }

  Result<void> dispatch(Control event) {
    if (build_failure) {
      return Result<void>::failure(build_failure);
    }
    engine.eventBus()->publish("player.control", event);
    return event.error ? Result<void>::failure(event.error) : Result<void>::success();
  }

  Result<PlayerMediaInfo> open(const std::string& filename) {
    if (build_failure) {
      return Result<PlayerMediaInfo>::failure(build_failure);
    }
    auto valid = executor->status();
    if (!valid) {
      return Result<PlayerMediaInfo>::failure(valid.failure_record());
    }
    if (engine.state() != graphflow::core::LifecycleState::Created) {
      return Result<PlayerMediaInfo>::failure(make_failure(
          ErrorDomain::media, ErrorCode::invalid_state, "playback/open: graph already used"));
    }
    source->configure(filename);
    Failure failure;
    if (!engine.init()) {
      failure = node_failure("playback/prepare: graph init failed");
    } else {
      for (const auto& node : nodes) {
        auto prepared = node->prepare();
        if (!prepared) {
          failure = prepared.failure_record();
          break;
        }
      }
    }
    if (failure) {
      engine.release();
      edges.clear();
      return Result<PlayerMediaInfo>::failure(failure);
    }
    return Result<PlayerMediaInfo>::success(demux->info());
  }

  Result<void> flush_seek(int64_t position) {
    if (position < 0) {
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                                "playback/seek: negative position"));
    }
    return executor->reset(position);
  }

  Result<Step> step() {
    if (build_failure) {
      return Result<Step>::failure(build_failure);
    }
    if (engine.state() != graphflow::core::LifecycleState::Running) {
      return Result<Step>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                                "playback/step: graph is not running"));
    }
    if (cancelled.load()) {
      return Result<Step>::failure(
          make_failure(ErrorDomain::media, ErrorCode::cancelled, "playback: cancelled"));
    }
    auto stepped = executor->step();
    if (!stepped) {
      return Result<Step>::failure(stepped.failure_record());
    }
    switch (*stepped) {
      case MediaExecutor::Step::progress:
        return Result<Step>::success(Step::progress);
      case MediaExecutor::Step::backpressured:
        return Result<Step>::success(Step::backpressured);
      case MediaExecutor::Step::end:
        return Result<Step>::success(Step::end);
    }
    return Result<Step>::failure(make_failure(ErrorDomain::media, ErrorCode::internal_error,
                                              "playback: invalid executor result"));
  }

  Failure build_failure;
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

Result<PlayerMediaInfo> PlaybackGraph::open(const std::string& path) {
  return impl_->open(path);
}

Result<PlaybackGraph::Step> PlaybackGraph::step() {
  return impl_->step();
}

int64_t PlaybackGraph::end_us() const {
  return std::max(impl_->output->video->end_us(), impl_->output->audio->end_us());
}

std::shared_ptr<PlaybackOutputs> PlaybackGraph::outputs() const {
  return impl_->output;
}

std::vector<PlaybackGraph::NodeInfo> PlaybackGraph::nodes() const {
  std::vector<NodeInfo> result;
  for (const auto& node : impl_->nodes) {
    result.push_back({node->getName(), node->state(), node->received(), node->published()});
  }
  return result;
}

std::vector<graphflow::core::GraphEdge> PlaybackGraph::edges() const {
  return impl_->edges;
}

Result<void> PlaybackGraph::seek(int64_t position) {
  return impl_->dispatch(Impl::Control(Impl::Control::seek, position));
}

Result<void> PlaybackGraph::start() {
  return impl_->dispatch(Impl::Control(Impl::Control::start));
}

Result<void> PlaybackGraph::pause() {
  return impl_->dispatch(Impl::Control(Impl::Control::pause));
}
}  // namespace livekit::sdk::detail
