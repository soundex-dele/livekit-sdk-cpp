#include "engines/media/media_pipeline.hpp"
#include "media_graph/failure.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace livekit::sdk::client::detail {
namespace {
Result<void> validate_graph(const MediaGraph& graph, const graphflow::core::GraphNode* port,
                            bool publishing) {
  auto invalid = [](std::string reason) {
    return Result<void>::failure(
        make_failure(ErrorDomain::media, ErrorCode::invalid_argument, std::move(reason)));
  };
  if (graph.nodes.empty() || graph.nodes.size() > 128 || graph.connections.size() > 1024) {
    return invalid("Media graph requires 1..128 nodes and at most 1024 connections");
  }
  std::map<std::string, size_t> ids;
  size_t port_index = graph.nodes.size();
  for (size_t i = 0; i < graph.nodes.size(); ++i) {
    const auto& node = graph.nodes[i];
    if (!node || node->state() != graphflow::core::LifecycleState::Created ||
        node->getName().empty() || node->getName().size() > 256 ||
        !ids.emplace(node->getName(), i).second) {
      return invalid("Media graph nodes must be fresh with unique nonempty names");
    }
    const auto attached = [](const auto& edges) {
      return std::any_of(edges.begin(), edges.end(),
                         [](const auto& edge) { return edge.isConnection(); });
    };
    if (attached(node->getSubEdges()) || attached(node->getPubEdges())) {
      return invalid("Media graph node already belongs to another graph: " + node->getName());
    }
    if (node.get() == port) {
      port_index = i;
    }
  }
  if (port_index == graph.nodes.size()) {
    return invalid("Media graph must keep the supplied Room port");
  }
  const auto encoded = graphflow::core::detail::prettyName<rtc::nodes::EncodedFrameEvent>();
  const auto feedback = graphflow::core::detail::prettyName<rtc::nodes::SenderFeedbackEvent>();
  const auto keyframe = graphflow::core::detail::prettyName<rtc::nodes::KeyframeRequestEvent>();
  std::vector<std::vector<size_t>> neighbors(graph.nodes.size()), media(graph.nodes.size());
  std::vector<size_t> incoming(graph.nodes.size());
  std::set<std::tuple<std::string, std::string, std::string, std::string>> edges;
  bool media_port = false, feedback_port = !publishing;
  for (const auto& edge : graph.connections) {
    const auto from = ids.find(edge.from), to = ids.find(edge.to);
    if (from == ids.end() || to == ids.end() || edge.dataType.empty() ||
        !edges.emplace(edge.from, edge.to, edge.dataType, edge.condition).second) {
      return invalid("Media graph contains an unknown endpoint, untyped or duplicate connection");
    }
    const auto a = from->second, b = to->second;
    if (a == port_index || b == port_index) {
      if (!edge.condition.empty() ||
          (publishing && ((b == port_index && edge.dataType != encoded) ||
                          (a == port_index && edge.dataType != feedback))) ||
          (!publishing && ((a == port_index && edge.dataType != encoded) ||
                           (b == port_index && edge.dataType != keyframe)))) {
        return invalid("Media graph Room port has an invalid event direction or condition");
      }
      media_port |= edge.dataType == encoded;
      feedback_port |= edge.dataType == feedback;
    }
    neighbors[a].push_back(b);
    neighbors[b].push_back(a);
    if (edge.dataType != feedback && edge.dataType != keyframe) {
      media[a].push_back(b);
      ++incoming[b];
    }
  }
  if (!media_port || !feedback_port) {
    return invalid("Media graph requires an encoded Room connection and publisher feedback");
  }
  std::set<size_t> connected{port_index};
  std::vector<size_t> pending{port_index};
  while (!pending.empty()) {
    auto node = pending.back();
    pending.pop_back();
    for (auto next : neighbors[node]) {
      if (connected.insert(next).second) {
        pending.push_back(next);
      }
    }
  }
  if (connected.size() != graph.nodes.size()) {
    return invalid("Media graph contains nodes disconnected from the Room port");
  }
  for (size_t i = 0; i < incoming.size(); ++i) {
    if (!incoming[i]) {
      pending.push_back(i);
    }
  }
  size_t visited = 0;
  while (!pending.empty()) {
    auto node = pending.back();
    pending.pop_back();
    ++visited;
    for (auto next : media[node]) {
      if (!--incoming[next]) {
        pending.push_back(next);
      }
    }
  }
  return visited == graph.nodes.size() ? Result<void>::success()
                                       : invalid("Media graph media connections must be acyclic");
}

rtc::TrackDescriptor descriptor(const MediaPublishSpec& spec, const LocalTrack& track) {
  rtc::TrackDescriptor d;
  d.id = track.snapshot().id;
  d.name = spec.name;
  d.kind = static_cast<rtc::MediaKind>(spec.kind);
  d.format = {static_cast<rtc::Codec>(spec.format.codec),
              spec.format.clock_rate,
              spec.format.channels,
              spec.format.width,
              spec.format.height,
              spec.format.fmtp};
  d.queue = {spec.queue.max_frames, spec.queue.max_bytes, spec.queue.max_age};
  d.source = static_cast<rtc::PublicationSource>(spec.options.source);
  d.stream_group = spec.options.stream_group;
  d.follow_subscriber_demand = spec.options.follow_subscriber_demand;
  d.encodings.clear();
  for (const auto& e : spec.options.encodings) {
    d.encodings.push_back({e.id, e.max_bitrate_bps, e.active});
  }
  return d;
}

template <class To, class From>
To frame_copy(const From& value) {
  To frame;
  const auto bytes = value.buffer.bytes();
  frame.buffer = decltype(frame.buffer)(std::vector<uint8_t>(bytes.begin(), bytes.end()));
  frame.encoding_id = value.encoding_id;
  frame.timestamp = value.timestamp;
  frame.duration = value.duration;
  frame.captured = value.captured;
  frame.epoch = value.epoch;
  frame.config_revision = value.config_revision;
  frame.keyframe = value.keyframe;
  frame.sequence_number = value.sequence_number;
  frame.sender_capture_time_us = value.sender_capture_time_us;
  return frame;
}
}  // namespace

// The only transport bridge: Room handles, never legacy Endpoint/RoomEngine.
class MediaPort final : public graphflow::core::GraphNode {
 public:
  explicit MediaPort(LocalTrack track) : GraphNode("room.port", nullptr), local(std::move(track)) {}

  MediaPort() : GraphNode("room.port", nullptr) {}

  LocalTrack local;
  std::atomic<bool> keyframe{false};
  std::atomic<uint64_t> frames{0}, bytes{0};

  void receive(EncodedFrame value) {
    ++frames;
    bytes += value.buffer.size();
    publish(rtc::nodes::EncodedFrameEvent(frame_copy<rtc::EncodedFrame>(value)));
  }

  void feedback(uint64_t revision) {
    auto current = local.feedback();
    const auto state = local.snapshot();
    rtc::SenderFeedback value;
    value.epoch = current.epoch;
    value.revision = revision;
    value.media_generation = current.media_generation;
    for (const auto& [id, e] : current.encodings) {
      value.encodings[id] = {e.target_bitrate_bps,
                             e.active && state.state == TrackState::ready && !state.muted,
                             e.keyframe_required, e.keyframe_requests};
    }
    publish(rtc::nodes::SenderFeedbackEvent(std::move(value)));
  }

 private:
  bool doInit() override {
    if (local) {
      subscribe<rtc::nodes::EncodedFrameEvent>([this](const auto& event) {
        if (local.try_write(frame_copy<EncodedFrame>(event.frame)) == WriteStatus::accepted) {
          ++frames;
          bytes += event.frame.buffer.size();
        }
      });
    } else {
      subscribe<rtc::nodes::KeyframeRequestEvent>([this](const auto&) { keyframe = true; });
    }
    return true;
  }
};

MediaPipeline::~MediaPipeline() {
  close();
}

void MediaPipeline::fail(Failure failure) {
  std::lock_guard lock(state_mutex_);
  if (!failure_) {
    failure_ = std::move(failure);
  }
}

void MediaPipeline::outputs(const std::shared_ptr<graphflow::core::GraphNode>& source,
                            MediaOutput options, const rtc::TrackDescriptor& d,
                            std::shared_ptr<rtc::AudioOutputSession> device) {
  output_ = std::make_shared<rtc::engine::ApplicationOutput>(options);
  nodes_.push_back(output_);
  auto wire = [&](const auto& target) {
    if (d.kind == rtc::MediaKind::video) {
      connect<rtc::nodes::VideoFrameEvent>(source, target);
    } else {
      connect<rtc::nodes::AudioFrameEvent>(source, target);
    }
  };
  wire(output_);
  if (options.playout) {
    speaker_ = std::make_shared<rtc::nodes::AudioOutputNode>("speaker", d.id, std::move(device));
    nodes_.push_back(speaker_);
    wire(speaker_);
  }
  if (!options.recording.empty()) {
    recording_ =
        std::make_shared<rtc::nodes::RecordingNode>("recording", options.recording, d.id, d.kind);
    nodes_.push_back(recording_);
    wire(recording_);
  }
}

Result<void> MediaPipeline::build(const std::shared_ptr<const MediaGraphOptions>& options,
                                  const rtc::TrackDescriptor& descriptor) {
  MediaGraph candidate;
  candidate.nodes = nodes_;
  candidate.connections = edges_;
  if (options) {
    if (!options->build) {
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                                "Media graph builder is empty"));
    }
    MediaGraphContext context;
    context.publishing = publishing_;
    context.track = descriptor;
    context.port = port_->getName();
    context.input = input_ ? input_->getName() : "";
    context.codec = video_ ? video_->getName() : audio_ ? audio_->getName() : "";
    context.output = output_ ? output_->getName() : "";
    context.defaults = std::move(candidate);
    rtc::detail::CallbackScope callback_scope;
    auto result = options->build(std::move(context));
    if (!result) {
      return Result<void>::failure(result.failure_record());
    }
    candidate = std::move(*result);
  }
  // Serialize fresh-node validation and adoption across MediaEngine instances.
  // A second builder cannot claim the same Created node before init changes it.
  static std::mutex adoption;
  std::lock_guard lock(adoption);
  if (auto valid = validate_graph(candidate, port_.get(), publishing_); !valid) {
    return valid;
  }
  nodes_ = std::move(candidate.nodes);
  edges_ = std::move(candidate.connections);
  custom_activate_ = std::move(candidate.activate);
  custom_quiesce_ = std::move(candidate.quiesce);
  auto retain = [&](auto& node) {
    if (node && std::find(nodes_.begin(), nodes_.end(), node) == nodes_.end()) {
      node.reset();
    }
  };
  retain(input_);
  retain(video_);
  retain(audio_);
  retain(output_);
  retain(speaker_);
  retain(recording_);
  for (auto& node : nodes_) {
    FailureContext context;
    context.track_id = descriptor.id;
    context.node = node->getName();
    node->setErrorHandler([weak = weak_from_this(), context](const auto& error) {
      if (auto self = weak.lock()) {
        self->fail(livekit::sdk::detail::graph_failure(error, context));
      }
    });
  }
  graph_ = std::make_unique<graphflow::engine::GraphEngine>(1);
  rtc::detail::CallbackScope callback_scope;
  if (!graph_->loadFromNodes(nodes_, edges_) || !graph_->init()) {
    auto value = snapshot().failure;
    return Result<void>::failure(value ? value
                                       : make_failure(ErrorDomain::media,
                                                      ErrorCode::processing_failed,
                                                      "Cannot initialize media graph"));
  }
  return Result<void>::success();
}

Result<void> MediaPipeline::prepare(const MediaPublishSpec& spec, const LocalTrack& track,
                                    std::shared_ptr<rtc::AudioOutputSession> device) {
  publishing_ = true;
  local = track;
  auto d = descriptor(spec, track);
  rtc::nodes::MediaInputOptions input;
  input.generated = spec.input.generated;
  input.loop = spec.input.loop;
  input.input = spec.input.input;
  input.ffmpeg = spec.input.ffmpeg;
  input.capture = spec.input.capture;
  input.device_index = spec.input.device_index;
  input.screen_id = spec.input.screen_id;
  input.fps = spec.input.fps;
  input.audio_processing = spec.input.audio_processing;
  input.echo_reference = spec.input.echo_reference;
  input.video_source = spec.input.video_source;
  input.audio_source = spec.input.audio_source;
  input.encoded_source = spec.input.encoded_source;
  if (spec.input.microphone) {
    input.microphone = std::make_shared<MicrophoneSession>(*spec.input.microphone);
    if (!input.audio_processing) {
      input.audio_processing = rtc::AudioProcessingOptions{};
    }
  }
  input_ = std::make_shared<rtc::nodes::MediaInputNode>("capture", d, std::move(input));
  port_ = std::make_shared<MediaPort>(track);
  nodes_ = {port_, input_};
  connect<rtc::nodes::SenderFeedbackEvent>(port_, input_);
  if (spec.input.encoded_source) {
    connect<rtc::nodes::EncodedFrameEvent>(input_, port_);
  } else if (spec.kind == MediaKind::video) {
    auto config = spec.video;
    config.fps = spec.input.fps;
    video_ = std::make_shared<rtc::nodes::VideoCodecNode>("encoder", d, true, config);
    nodes_.push_back(video_);
    connect<rtc::nodes::VideoFrameEvent>(input_, video_);
    connect<rtc::nodes::EncodedFrameEvent>(video_, port_);
    connect<rtc::nodes::SenderFeedbackEvent>(port_, video_);
  } else {
    audio_ = std::make_shared<rtc::nodes::AudioCodecNode>("encoder", d, true, spec.audio);
    nodes_.push_back(audio_);
    connect<rtc::nodes::AudioFrameEvent>(input_, audio_);
    connect<rtc::nodes::EncodedFrameEvent>(audio_, port_);
    connect<rtc::nodes::SenderFeedbackEvent>(port_, audio_);
  }
  if (!spec.input.encoded_source) {
    outputs(input_, spec.preview, d, std::move(device));
  }
  return build(spec.graph, d);
}

Result<void> MediaPipeline::prepare(const MediaSubscribeSpec& spec, const RemoteTrack& track,
                                    rtc::TrackDescriptor d,
                                    std::shared_ptr<rtc::AudioOutputSession> device,
                                    std::shared_ptr<rtc::detail::MediaSyncClock> clock) {
  remote = track;
  subscription = spec.publication;
  port_ = std::make_shared<MediaPort>();
  nodes_.push_back(port_);
  std::shared_ptr<graphflow::core::GraphNode> raw;
  if (d.kind == rtc::MediaKind::video) {
    raw = video_ = std::make_shared<rtc::nodes::VideoCodecNode>("decoder", d, false, spec.video);
    connect<rtc::nodes::KeyframeRequestEvent>(video_, port_);
  } else {
    raw = audio_ = std::make_shared<rtc::nodes::AudioCodecNode>("decoder", d, false, spec.audio);
  }
  nodes_.push_back(raw);
  connect<rtc::nodes::EncodedFrameEvent>(port_, raw);
  if (clock) {
    auto sync = std::make_shared<rtc::engine::MediaSyncNode>(std::move(clock));
    nodes_.push_back(sync);
    if (d.kind == rtc::MediaKind::video) {
      connect<rtc::nodes::VideoFrameEvent>(raw, sync);
    } else {
      connect<rtc::nodes::AudioFrameEvent>(raw, sync);
    }
    raw = sync;
  }
  outputs(raw, spec.output, d, std::move(device));
  return build(spec.graph, d);
}

Result<void> MediaPipeline::start() {
  rtc::detail::CallbackScope callback_scope;
  if (!graph_->start()) {
    auto error = snapshot().failure;
    return Result<void>::failure(error ? error
                                       : make_failure(ErrorDomain::media,
                                                      ErrorCode::processing_failed,
                                                      "Cannot start media graph"));
  }
  for (const auto& node : nodes_) {
    if (auto input = std::dynamic_pointer_cast<rtc::nodes::MediaInputNode>(node)) {
      if (auto result = input->activate(); !result) {
        return result;
      }
    }
  }
  if (custom_activate_ && !custom_activate_()) {
    auto error = snapshot().failure;
    return Result<void>::failure(error ? error
                                       : make_failure(ErrorDomain::media,
                                                      ErrorCode::processing_failed,
                                                      "Cannot activate custom media graph"));
  }
  if (auto error = snapshot().failure) {
    return Result<void>::failure(error);
  }
  pump_ = std::thread([this] {
    uint64_t revision = 0;
    auto last_keyframe = SteadyClock::time_point{};
    while (!stopping_) {
      const auto state = publishing_ ? local.snapshot() : remote.snapshot();
      Failure failure;
      {
        std::lock_guard lock(state_mutex_);
        failure = failure_;
      }
      if (state.state == TrackState::closed || state.state == TrackState::failed || failure) {
        break;
      }
      if (publishing_) {
        port_->feedback(++revision);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      } else {
        auto value = remote.read({std::chrono::milliseconds(20), {}});
        if (value.status == ReadStatus::frame) {
          port_->receive(std::move(*value.frame));
        } else if (value.status == ReadStatus::failed) {
          fail(value.failure);
          break;
        } else if (value.status == ReadStatus::suspended) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (port_->keyframe &&
            SteadyClock::now() - last_keyframe > std::chrono::milliseconds(500)) {
          port_->keyframe = false;
          remote.request_keyframe();
          last_keyframe = SteadyClock::now();
        }
      }
    }
    stopping_ = true;
    stop_nodes();
  });
  return Result<void>::success();
}

void MediaPipeline::stop_nodes() {
  std::lock_guard lock(lifecycle_);
  if (stopped_) {
    return;
  }
  stopped_ = true;
  rtc::detail::CallbackScope callback_scope;
  for (const auto& node : nodes_) {
    if (auto input = std::dynamic_pointer_cast<rtc::nodes::MediaInputNode>(node)) {
      input->quiesce();
    }
  }
  if (custom_quiesce_) {
    custom_quiesce_();
  }
  for (const auto& node : nodes_) {
    if (auto video = std::dynamic_pointer_cast<rtc::nodes::VideoCodecNode>(node)) {
      video->quiesce();
    }
    if (auto audio = std::dynamic_pointer_cast<rtc::nodes::AudioCodecNode>(node)) {
      audio->quiesce();
    }
  }
  for (const auto& node : nodes_) {
    if (auto output = std::dynamic_pointer_cast<rtc::engine::ApplicationOutput>(node)) {
      output->quiesce();
    }
    if (auto speaker = std::dynamic_pointer_cast<rtc::nodes::AudioOutputNode>(node)) {
      speaker->quiesce();
    }
    if (auto recording = std::dynamic_pointer_cast<rtc::nodes::RecordingNode>(node)) {
      recording->quiesce();
    }
  }
  if (graph_) {
    graph_->stop();
    graph_->release();
  }
}

void MediaPipeline::close() {
  stopping_ = true;
  if (pump_.joinable()) {
    pump_.join();
  }
  stop_nodes();
}

MediaTrackSnapshot MediaPipeline::snapshot() const {
  MediaTrackSnapshot value;
  value.transport = publishing_ ? local.snapshot() : remote.snapshot();
  value.frames = port_ ? port_->frames.load() : 0;
  value.bytes = port_ ? port_->bytes.load() : 0;
  auto row = [&](const auto& node, const char* kind, uint64_t in, uint64_t out, uint64_t dropped) {
    value.nodes.push_back({node->getName(), kind, in, out, dropped, 0});
  };
  if (input_) {
    auto s = input_->snapshot();
    value.input = s;
    row(input_, "MediaInputNode", s.emitted + s.dropped, s.emitted, s.dropped);
  }
  if (video_) {
    auto s = video_->snapshot();
    row(video_, publishing_ ? "VideoEncoderNode" : "VideoDecoderNode", s.admitted, s.completed,
        s.dropped);
  }
  if (audio_) {
    auto s = audio_->snapshot();
    row(audio_, publishing_ ? "AudioEncoderNode" : "AudioDecoderNode", s.admitted, s.completed,
        s.dropped);
  }
  if (port_) {
    row(port_, publishing_ ? "RoomPublishPort" : "RoomSubscribePort", value.frames, value.frames,
        0);
  }
  if (output_) {
    const auto frames = output_->frames.load();
    const auto dropped = output_->dropped.load();
    row(output_, "ApplicationOutputNode", frames + dropped, frames, dropped);
    value.nodes.back().errors = output_->status() ? 0 : 1;
  }
  if (recording_) {
    const auto state = recording_->snapshot();
    row(recording_, "RecordingNode", state.admitted, state.written, 0);
    value.nodes.back().errors = state.error.empty() ? 0 : 1;
  }
  for (const auto& node : nodes_) {
    if (std::none_of(value.nodes.begin(), value.nodes.end(),
                     [&](const auto& item) { return item.name == node->getName(); })) {
      row(node, "GraphNode", 0, 0, 0);
      value.nodes.back().errors = node->error() ? 1 : 0;
    }
  }
  for (const auto& edge : edges_) {
    value.edges.push_back({edge.from, edge.to, edge.dataType});
  }
  std::lock_guard lock(state_mutex_);
  value.failure = failure_ ? failure_ : value.transport.failure;
  if (value.failure) {
    value.transport.state = TrackState::failed;
    value.transport.failure = value.failure;
    value.transport.error = value.failure->message;
  } else if (stopping_) {
    value.transport.state = TrackState::closed;
  }
  return value;
}
}  // namespace livekit::sdk::client::detail
