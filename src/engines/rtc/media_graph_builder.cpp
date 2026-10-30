#include "engines/rtc/media_graph_builder.hpp"
#include "engine/graph_engine.h"
#include "engines/rtc/application_output.hpp"
#include "engines/rtc/sync_node.hpp"
#include "media_graph/raw_frame_queue.hpp"
#include "backends/rtc/control_context.hpp"
#include <atomic>
#include <algorithm>
#include "livekit/rtc/input_node.hpp"
#include "livekit/rtc/recording_node.hpp"
#include <stdexcept>

namespace livekit::sdk::rtc::engine {
namespace {
void validate_output(MediaKind kind, const MediaOutput& output) {
  if ((kind == MediaKind::video && (output.audio || output.playout)) ||
      (kind == MediaKind::audio && output.video))
    throw std::invalid_argument("Media output does not match track kind");
}

nodes::MediaInputOptions input_options(const MediaInput& source) {
  nodes::MediaInputOptions result;
  result.generated = source.generated;
  result.loop = source.loop;
  result.input = source.input;
  result.ffmpeg = source.ffmpeg;
  result.capture = source.capture;
  result.device_index = source.device_index;
  result.screen_id = source.screen_id;
  result.fps = source.fps;
  result.audio_processing = source.audio_processing;
  result.echo_reference = source.echo_reference;
  if (source.microphone && !result.audio_processing) {
    result.audio_processing = AudioProcessingOptions{};
  }
  result.video_source = source.video_source;
  result.audio_source = source.audio_source;
  result.encoded_source = source.encoded_source;
  if (source.microphone)
    result.microphone = std::make_shared<MicrophoneSession>(*source.microphone);
  return result;
}
}  // namespace

struct SharedMediaInput::Impl : std::enable_shared_from_this<SharedMediaInput::Impl> {
  struct Target {
    std::shared_ptr<detail::RawFrameQueue> frames;
    graphflow::core::GraphNode::FailureHandler report;
    bool active = false;  // lifecycle mutex
  };

  struct Feedback final : graphflow::core::GraphNode {
    Feedback() : GraphNode("shared.feedback", nullptr) {}

    void start_input() {
      SenderFeedback value;
      value.epoch = 1;
      value.revision = 1;
      publish(nodes::SenderFeedbackEvent(value));
    }
  };

  TrackDescriptor format;
  MediaInput options;
  mutable std::mutex lifecycle, targets_mutex;
  std::vector<std::weak_ptr<Target>> targets;
  std::exception_ptr failure;  // targets_mutex
  size_t consumers = 0;
  uint64_t emitted = 0;
  std::unique_ptr<graphflow::engine::GraphEngine> graph;
  std::shared_ptr<nodes::MediaInputNode> input;

  std::vector<std::shared_ptr<Target>> receivers() {
    std::lock_guard lock(targets_mutex);
    std::vector<std::shared_ptr<Target>> result;
    for (auto& weak : targets)
      if (auto target = weak.lock())
        result.push_back(std::move(target));
    return result;
  }

  void report_failure(std::exception_ptr error) {
    error = normalized_failure(error, {"shared_capture", {}, "shared.capture", 0},
                               ErrorDomain::media, ErrorCode::processing_failed);
    {
      std::lock_guard lock(targets_mutex);
      if (failure) {
        return;
      }
      failure = error;
    }
    for (const auto& target : receivers()) {
      if (target->report) {
        target->report(error);
      }
    }
  }

  void stop_capture() {
    if (input) {
      input->quiesce();
      emitted += input->snapshot().emitted;
    }
    if (graph) {
      graph->stop();
      graph->release();
    }
    input.reset();
    graph.reset();
  }

  void start(const std::shared_ptr<Target>& target) {
    std::lock_guard lock(lifecycle);
    if (target->active)
      return;
    if (consumers >= 128)
      throw std::runtime_error("Shared capture consumer limit reached");
    {
      std::lock_guard lock(targets_mutex);
      if (!consumers) {
        failure = {};
      }
      if (failure) {
        std::rethrow_exception(failure);
      }
      targets.push_back(target);
    }
    try {
      if (!consumers) {
        input = std::make_shared<nodes::MediaInputNode>("shared.capture", format,
                                                        input_options(options));
        auto feedback = std::make_shared<Feedback>();
        MediaOutput delivery;
        if (format.kind == MediaKind::video)
          delivery.video = [this](const auto& sample) {
            for (auto& receiver : receivers())
              receiver->frames->write(
                  {sample.pixels, sample.captured,
                   sample.pixels->y.size() + sample.pixels->u.size() + sample.pixels->v.size()});
          };
        else
          delivery.audio = [this](const auto& sample) {
            for (auto& receiver : receivers())
              receiver->frames->write({sample.pcm, sample.captured,
                                       sample.pcm->samples.size() * sizeof(int16_t) +
                                           sample.pcm->float_samples.size() * sizeof(float),
                                       0, sample.timestamp});
          };
        auto output = std::make_shared<ApplicationOutput>(std::move(delivery), true);
        std::vector<graphflow::engine::NodeConnection> edges = {
            TrackGraphSpec::connect<nodes::SenderFeedbackEvent>(feedback, input)};
        if (format.kind == MediaKind::video)
          edges.push_back(TrackGraphSpec::connect<nodes::VideoFrameEvent>(input, output));
        else
          edges.push_back(TrackGraphSpec::connect<nodes::AudioFrameEvent>(input, output));
        auto report = [weak = weak_from_this()](auto error) {
          if (auto state = weak.lock()) {
            state->report_failure(error);
          }
        };
        input->setFailureHandler(report);
        output->setFailureHandler(report);
        feedback->setFailureHandler(report);
        graph = std::make_unique<graphflow::engine::GraphEngine>(1);
        if (!graph->loadFromNodes({output, input, feedback}, edges) || !graph->init() ||
            !graph->start())
          throw std::runtime_error("Unable to start shared capture graph");
        feedback->start_input();
        input->activate();
      }
      target->active = true;
      ++consumers;
    } catch (...) {
      {
        std::lock_guard lock(targets_mutex);
        std::erase_if(targets, [&](auto& weak) { return weak.lock() == target; });
      }
      if (!consumers)
        stop_capture();
      throw;
    }
  }

  void stop(const std::shared_ptr<Target>& target) {
    std::lock_guard lock(lifecycle);
    if (!target->active)
      return;
    {
      std::lock_guard lock(targets_mutex);
      std::erase_if(targets, [&](auto& weak) { return weak.expired() || weak.lock() == target; });
    }
    target->active = false;
    if (--consumers == 0)
      stop_capture();
  }

  ~Impl() {
    stop_capture();
  }
};

SharedMediaInput::~SharedMediaInput() = default;

SharedInputSnapshot SharedMediaInput::snapshot() const {
  std::lock_guard lock(impl_->lifecycle);
  return {impl_->consumers, bool(impl_->graph),
          impl_->emitted + (impl_->input ? impl_->input->snapshot().emitted : 0)};
}

struct MediaTrack::State {
  mutable std::mutex mutex;
  TrackDescriptor descriptor;
  bool publication = false;
  std::exception_ptr failure;
  std::shared_ptr<PublishedTrack> published;
  std::shared_ptr<SubscribedTrack> subscribed;
  std::shared_ptr<nodes::MediaInputNode> input;
  std::shared_ptr<nodes::VideoCodecNode> video;
  std::shared_ptr<nodes::AudioCodecNode> audio;
  std::shared_ptr<nodes::RtcPublishNode> sink;
  std::shared_ptr<nodes::RtcSubscribeNode> source;
  std::shared_ptr<ApplicationOutput> output;
  std::shared_ptr<nodes::AudioOutputNode> speaker;
  std::shared_ptr<nodes::RecordingNode> recording;
  std::vector<MediaEdgeSnapshot> edges;
  std::function<void()> start_source, stop_source;

  void check() const {
    std::lock_guard lock(mutex);
    if (failure) {
      std::rethrow_exception(failure);
    }
  }

  void failed(std::exception_ptr value) {
    std::lock_guard lock(mutex);
    if (!failure) {
      failure = value;
    }
  }

  // Fields are installed before graph start and never replaced. Never hold
  // the snapshot mutex while joining a worker that may call an application.
  void quiesce() {
    if (input)
      input->quiesce();
    if (video)
      video->quiesce();
    if (audio)
      audio->quiesce();
    if (output) {
      output->quiesce();
    }
    if (speaker)
      speaker->quiesce();
    if (recording)
      recording->quiesce();
    if (stop_source)
      stop_source();
  }

  template <class Event>
  void connect(TrackGraphSpec& graph, const auto& from, const auto& to) {
    graph.connections.push_back(TrackGraphSpec::connect<Event>(from, to));
  }

  void outputs(TrackGraphSpec& graph, const std::shared_ptr<graphflow::core::GraphNode>& raw,
               MediaOutput options, const std::shared_ptr<AudioOutputSession>& device) {
    output = std::make_shared<ApplicationOutput>(options);
    graph.nodes.push_back(output);
    if (descriptor.kind == MediaKind::video)
      connect<nodes::VideoFrameEvent>(graph, raw, output);
    else
      connect<nodes::AudioFrameEvent>(graph, raw, output);
    if (options.playout) {
      speaker = std::make_shared<nodes::AudioOutputNode>("speaker", descriptor.id, device);
      graph.nodes.push_back(speaker);
      connect<nodes::AudioFrameEvent>(graph, raw, speaker);
    }
    if (!options.recording.empty()) {
      recording = std::make_shared<nodes::RecordingNode>(
          "recording", options.recording,
          descriptor.remote_sid.empty() ? descriptor.id : descriptor.remote_sid, descriptor.kind);
      graph.nodes.push_back(recording);
      if (descriptor.kind == MediaKind::video)
        connect<nodes::VideoFrameEvent>(graph, raw, recording);
      else
        connect<nodes::AudioFrameEvent>(graph, raw, recording);
    }
  }

  void topology(const TrackGraphSpec& graph) {
    for (const auto& edge : graph.connections)
      edges.push_back({edge.from, edge.to, edge.dataType});
  }
};

MediaTrackSnapshot MediaTrack::snapshot() const {
  const auto& s = *state_;
  std::lock_guard lock(s.mutex);
  MediaTrackSnapshot result;
  result.transport = s.published    ? s.published->snapshot()
                     : s.subscribed ? s.subscribed->snapshot()
                                    : TrackSnapshot{};
  if (!s.published && !s.subscribed)
    result.transport.id = s.descriptor.id;
  if (s.failure) {
    result.transport.state = TrackState::failed;
    result.transport.failure = describe_failure(s.failure);
    try {
      std::rethrow_exception(s.failure);
    } catch (const std::exception& error) {
      result.transport.error = error.what();
    } catch (...) {
      result.transport.error = "Media operation failed";
    }
  }
  const auto row = [&](const auto& node, const char* kind, uint64_t in, uint64_t out,
                       uint64_t dropped = 0, uint64_t errors = 0) {
    result.nodes.push_back({node->getName(), kind, in, out, dropped, errors});
  };
  if (s.input) {
    auto v = s.input->snapshot();
    result.input = v;
    row(s.input, "MediaInputNode", v.emitted + v.dropped, v.emitted, v.dropped, !v.error.empty());
  }
  if (s.video) {
    auto v = s.video->snapshot();
    result.video = v;
    row(s.video, s.publication ? "VideoEncoderNode" : "VideoDecoderNode", v.admitted, v.completed,
        v.dropped, !v.error.empty());
  }
  if (s.audio) {
    auto v = s.audio->snapshot();
    result.audio = v;
    row(s.audio, s.publication ? "AudioEncoderNode" : "AudioDecoderNode", v.admitted, v.completed,
        v.dropped, !v.error.empty());
  }
  if (s.sink) {
    row(s.sink, "RtcPublishNode", s.sink->accepted() + s.sink->rejected(), s.sink->accepted(),
        s.sink->rejected());
    result.frames = s.sink->accepted();
  }
  if (s.source)
    row(s.source, "RtcSubscribeNode", s.source->delivered(), s.source->delivered());
  if (s.output) {
    // Read drops first: every drop follows its frame admission. Independent
    // reads in the opposite order can underflow while producers are active.
    const auto dropped = s.output->dropped.load();
    const auto admitted = s.output->frames.load();
    row(s.output, "ApplicationOutputNode", admitted, admitted - dropped, dropped);
    if (!s.publication)
      result.frames = s.output->frames;
    result.bytes = s.output->bytes;
  }
  if (s.speaker) {
    auto v = s.speaker->snapshot();
    row(s.speaker, "AudioOutputNode", v.admitted, v.rendered_samples, v.dropped);
  }
  if (s.recording) {
    auto v = s.recording->snapshot();
    row(s.recording, "RecordingNode", v.admitted, v.written, 0, !v.error.empty());
  }
  result.edges = s.edges;
  return result;
}

Result<void> MediaTrack::check() const {
  return detail::control_boundary([&] { state_->check(); });
}

PreparedMediaGraph RtcMediaGraphBuilder::build_publish(
    MediaPublishSpec spec, std::shared_ptr<nodes::RtcPublishNode> sink) {
  validate_output(spec.track.kind, spec.preview);
  if (!spec.input.echo_reference) {
    spec.input.echo_reference = resources_.echo_reference;
  }
  if (spec.input.encoded_source &&
      (spec.preview.video || spec.preview.audio || spec.preview.frame || spec.preview.playout ||
       !spec.preview.recording.empty())) {
    throw std::invalid_argument(
        "Encoded publication has no raw preview; subscribe with a decoder to consume pixels or "
        "PCM");
  }
  auto state = std::make_shared<MediaTrack::State>();
  state->descriptor = spec.track;
  state->publication = true;
  std::shared_ptr<detail::RawFrameQueue> shared_frames;
  std::shared_ptr<SharedMediaInput::Impl::Target> shared_target;
  if (spec.shared_input) {
    if (spec.input.generated || !spec.input.input.empty() ||
        spec.input.capture != CaptureKind::input || spec.input.video_source ||
        spec.input.audio_source || spec.input.encoded_source || spec.input.microphone)
      throw std::invalid_argument("Choose a shared input or an individual input");
    auto shared = spec.shared_input->impl_;
    const auto& format = shared->format;
    if (format.kind != spec.track.kind ||
        (format.kind == MediaKind::video && (format.format.width != spec.track.format.width ||
                                             format.format.height != spec.track.format.height)) ||
        (format.kind == MediaKind::audio && format.format.channels != spec.track.format.channels))
      throw std::invalid_argument("Shared input raw format must match publication");
    auto target = shared_target = std::make_shared<SharedMediaInput::Impl::Target>();
    spec.input.fps = shared->options.fps;
    QueueLimits limits;
    limits.max_frames =
        format.kind == MediaKind::video ? spec.video.max_frames : spec.audio.max_frames;
    limits.max_bytes =
        format.kind == MediaKind::video ? spec.video.max_bytes : spec.audio.max_bytes;
    limits.max_age = format.kind == MediaKind::video ? spec.video.max_age : spec.audio.max_age;
    shared_frames = target->frames = std::make_shared<detail::RawFrameQueue>(limits);
    state->start_source = [shared, target] { shared->start(target); };
    state->stop_source = [shared, target] { shared->stop(target); };
  }
  auto source_options = input_options(spec.input);
  source_options.shared_frames = std::move(shared_frames);
  state->input =
      std::make_shared<nodes::MediaInputNode>("input", spec.track, std::move(source_options));
  if (shared_target) {
    shared_target->report = state->input->failureReporter();
  }
  try {
    std::lock_guard lock(state->mutex);
    state->sink = sink;
    state->published = sink->track();
    TrackGraphSpec graph;
    graph.nodes.push_back(state->input);
    if (spec.input.encoded_source)
      state->connect<nodes::EncodedFrameEvent>(graph, state->input, sink);
    else if (spec.track.kind == MediaKind::video) {
      auto config = spec.video;
      config.fps = spec.input.fps;
      state->video =
          std::make_shared<nodes::VideoCodecNode>("video.encoder", spec.track, true, config);
      graph.nodes.push_back(state->video);
      state->connect<nodes::VideoFrameEvent>(graph, state->input, state->video);
      state->connect<nodes::EncodedFrameEvent>(graph, state->video, sink);
      state->connect<nodes::SenderFeedbackEvent>(graph, sink, state->video);
    } else {
      state->audio =
          std::make_shared<nodes::AudioCodecNode>("audio.encoder", spec.track, true, spec.audio);
      graph.nodes.push_back(state->audio);
      state->connect<nodes::AudioFrameEvent>(graph, state->input, state->audio);
      state->connect<nodes::EncodedFrameEvent>(graph, state->audio, sink);
      state->connect<nodes::SenderFeedbackEvent>(graph, sink, state->audio);
    }
    if (spec.preview.video || spec.preview.audio || spec.preview.frame || spec.preview.playout ||
        !spec.preview.recording.empty()) {
      state->outputs(graph, state->input, spec.preview, resources_.output);
    }
    state->connect<nodes::SenderFeedbackEvent>(graph, sink, state->input);
    state->topology(graph);
    graph.activate = [state] {
      state->input->activate();
      if (state->start_source)
        state->start_source();
    };
    graph.quiesce = [state] { state->quiesce(); };
    graph.on_failure = [state](auto error) { state->failed(error); };
    return {std::move(graph), std::shared_ptr<MediaTrack>(new MediaTrack(state))};
  } catch (...) {
    state->quiesce();
    throw;
  }
}

PreparedMediaGraph RtcMediaGraphBuilder::build_subscribe(
    MediaSubscribeSpec spec, std::shared_ptr<nodes::RtcSubscribeNode> source) {
  validate_output(spec.track.kind, spec.output);
  if (spec.presentation_delay.count() < 0 ||
      spec.presentation_delay > std::chrono::milliseconds(500)) {
    throw std::invalid_argument("Invalid presentation delay");
  }
  std::shared_ptr<detail::MediaSyncClock> clock;
  if (spec.synchronize && !spec.track.source_id.empty()) {
    std::erase_if(resources_.clocks, [](const auto& entry) { return entry.second.expired(); });
    const auto key = std::pair{spec.track.source_id, spec.track.stream_group};
    clock = resources_.clocks[key].lock();
    if (!clock) {
      resources_.clocks[key] = clock =
          std::make_shared<detail::MediaSyncClock>(spec.presentation_delay);
    }
  }
  auto state = std::make_shared<MediaTrack::State>();
  state->descriptor = spec.track;
  try {
    std::lock_guard lock(state->mutex);
    state->source = source;
    state->subscribed = source->track();
    TrackGraphSpec graph;
    std::shared_ptr<graphflow::core::GraphNode> decoder;
    if (spec.track.kind == MediaKind::video) {
      state->video =
          std::make_shared<nodes::VideoCodecNode>("video.decoder", spec.track, false, spec.video);
      decoder = state->video;
      state->connect<nodes::KeyframeRequestEvent>(graph, decoder, source);
    } else {
      state->audio =
          std::make_shared<nodes::AudioCodecNode>("audio.decoder", spec.track, false, spec.audio);
      decoder = state->audio;
    }
    graph.nodes.push_back(decoder);
    state->connect<nodes::EncodedFrameEvent>(graph, source, decoder);
    if (clock) {
      auto sync = std::make_shared<MediaSyncNode>(clock);
      graph.nodes.push_back(sync);
      if (spec.track.kind == MediaKind::video) {
        state->connect<nodes::VideoFrameEvent>(graph, decoder, sync);
      } else {
        state->connect<nodes::AudioFrameEvent>(graph, decoder, sync);
      }
      state->outputs(graph, sync, spec.output, resources_.output);
    } else {
      state->outputs(graph, decoder, spec.output, resources_.output);
    }
    state->topology(graph);
    graph.quiesce = [state] { state->quiesce(); };
    graph.on_failure = [state](auto error) { state->failed(error); };
    return {std::move(graph), std::shared_ptr<MediaTrack>(new MediaTrack(state))};
  } catch (...) {
    state->quiesce();
    throw;
  }
}

std::shared_ptr<SharedMediaInput> RtcMediaGraphBuilder::create_shared_input(TrackDescriptor format,
                                                                            MediaInput options) {
  if (options.encoded_source || !options.input.empty())
    throw std::invalid_argument("Shared capture requires a live raw source");
  format.follow_subscriber_demand = false;
  if (!options.echo_reference) {
    options.echo_reference = resources_.echo_reference;
  }
  // Validate before a publication exists; constructing a source opens no device.
  {
    nodes::MediaInputNode validate("shared.validate", format, input_options(options));
  }
  auto state = std::make_shared<SharedMediaInput::Impl>();
  state->format = std::move(format);
  state->options = std::move(options);
  return std::shared_ptr<SharedMediaInput>(new SharedMediaInput(std::move(state)));
}
}  // namespace livekit::sdk::rtc::engine
