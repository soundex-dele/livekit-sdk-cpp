#include "engines/player/playback_graph.hpp"
#include "backends/ffmpeg/ffmpeg_playback.hpp"
#include <algorithm>

namespace livekit::sdk::detail {
class PlaybackGraph::Impl {
 public:
  Impl(std::atomic<bool>& cancel, const PlayerOptions& options)
      : cancelled(cancel),
        demux(std::make_shared<DemuxerNode>(cancel, options.io_timeout)),
        output(std::make_shared<PlaybackOutputs>(options)) {
    MediaBinding source(engine, demux), video(engine, std::make_shared<VideoDecoderNode>()),
        audio(engine, std::make_shared<AudioDecoderNode>()),
        rgba(engine, std::make_shared<VideoConverterNode>()),
        pcm(engine, std::make_shared<AudioResamplerNode>()), video_sink(engine, output->video),
        audio_sink(engine, output->audio);
    for (const auto* binding : {&source, &video, &audio, &rgba, &pcm, &video_sink, &audio_sink}) {
      if (!binding->ref) {
        build_failure = media_graph_failure(binding->ref.status());
        return;
      }
      nodes.push_back(binding->node);
    }
    std::vector<MediaEdge> candidate;
    auto wired = engine.rewire([&](graphflow::Wiring& w) {
      connect_media<VideoPacket>(w, candidate, source, video, "video");
      connect_media<AudioPacket>(w, candidate, source, audio, "audio");
      connect_media<DecodedVideo>(w, candidate, video, rgba);
      connect_media<DecodedAudio>(w, candidate, audio, pcm);
      connect_media<RGBA8>(w, candidate, rgba, video_sink);
      connect_media<StereoFloat48k>(w, candidate, pcm, audio_sink);
    });
    if (!wired) {
      build_failure = media_graph_failure(wired);
      return;
    }
    edges = std::move(candidate);
    executor = std::make_unique<MediaExecutor>(nodes, edges);
    nodes = executor->nodes();
  }

  ~Impl() {
    (void)engine.close();
  }

  Result<PlayerMediaInfo> open(const std::string& filename) {
    if (build_failure) {
      return Result<PlayerMediaInfo>::failure(build_failure);
    }
    auto valid = executor->status();
    if (!valid) {
      return Result<PlayerMediaInfo>::failure(valid.failure_record());
    }
    if (demux->state() != MediaState::Created) {
      return Result<PlayerMediaInfo>::failure(make_failure(
          ErrorDomain::media, ErrorCode::invalid_state, "playback/open: graph already used"));
    }
    demux->configure(filename);
    Failure failure;
    if (auto prepared = engine.start(graphflow::StartMode::PrepareOnly); !prepared) {
      failure = media_graph_failure(prepared);
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
      (void)engine.close();
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
    if (demux->state() == MediaState::Created || demux->state() == MediaState::Released) {
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                                "playback/seek: graph is not prepared"));
    }
    return executor->reset(position);
  }

  Result<Step> step(const MediaExecutor::PumpProbe& probe) {
    if (build_failure) {
      return Result<Step>::failure(build_failure);
    }
    if (demux->state() != MediaState::Running) {
      return Result<Step>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                                "playback/step: graph is not running"));
    }
    if (cancelled.load()) {
      return Result<Step>::failure(
          make_failure(ErrorDomain::media, ErrorCode::cancelled, "playback: cancelled"));
    }
    auto stepped = executor->step(probe);
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
  graphflow::Graph engine{{graphflow::Dispatch::Inline}};
  std::atomic<bool>& cancelled;
  std::shared_ptr<DemuxerNode> demux;
  std::shared_ptr<PlaybackOutputs> output;
  std::vector<std::shared_ptr<MediaNode>> nodes;
  std::vector<MediaEdge> edges;
  std::unique_ptr<MediaExecutor> executor;
};

PlaybackGraph::PlaybackGraph(std::atomic<bool>& cancel, const PlayerOptions& options)
    : impl_(std::make_unique<Impl>(cancel, options)) {}

PlaybackGraph::~PlaybackGraph() = default;

Result<PlayerMediaInfo> PlaybackGraph::open(const std::string& path) {
  return impl_->open(path);
}

Result<PlaybackGraph::Step> PlaybackGraph::step(const MediaExecutor::PumpProbe& probe) {
  return impl_->step(probe);
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

std::vector<MediaEdge> PlaybackGraph::edges() const {
  return impl_->edges;
}

Result<void> PlaybackGraph::seek(int64_t position) {
  if (impl_->build_failure) {
    return Result<void>::failure(impl_->build_failure);
  }
  return impl_->flush_seek(position);
}

Result<void> PlaybackGraph::start() {
  if (impl_->build_failure) {
    return Result<void>::failure(impl_->build_failure);
  }
  if (impl_->demux->state() == MediaState::Created ||
      impl_->demux->state() == MediaState::Released) {
    return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                              "playback/start: graph is not prepared"));
  }
  if (auto status = impl_->engine.start(); !status) {
    return Result<void>::failure(media_graph_failure(status));
  }
  return Result<void>::success();
}

Result<void> PlaybackGraph::pause() {
  if (impl_->build_failure) {
    return Result<void>::failure(impl_->build_failure);
  }
  if (auto status = impl_->engine.stop(); !status) {
    return Result<void>::failure(media_graph_failure(status));
  }
  return Result<void>::success();
}
}  // namespace livekit::sdk::detail
