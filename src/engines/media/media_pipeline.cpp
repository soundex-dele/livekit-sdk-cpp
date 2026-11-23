#include "engines/media/media_pipeline.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace livekit::sdk::client::detail {
namespace {
Result<void> validate_graph(const MediaGraph& graph, const livekit::sdk::rtc::TrackNode* port,
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
    if (!node || node->claimed() || node->name().empty() || node->name().size() > 256 ||
        !ids.emplace(node->name(), i).second) {
      return invalid("Media graph nodes must be fresh with unique nonempty names");
    }
    if (node.get() == port) {
      port_index = i;
    }
  }
  if (port_index == graph.nodes.size()) {
    return invalid("Media graph must keep the supplied Room port");
  }
  const auto encoded = std::string_view(rtc::TrackPort<rtc::nodes::EncodedFrameEvent>::out.name);
  const auto feedback = std::string_view(rtc::TrackPort<rtc::nodes::SenderFeedbackEvent>::out.name);
  const auto keyframe =
      std::string_view(rtc::TrackPort<rtc::nodes::KeyframeRequestEvent>::out.name);
  std::vector<std::vector<size_t>> neighbors(graph.nodes.size()), media(graph.nodes.size());
  std::vector<size_t> incoming(graph.nodes.size());
  std::set<std::tuple<std::string, std::string, std::string>> edges;
  bool media_port = false, feedback_port = !publishing;
  for (const auto& edge : graph.connections) {
    const auto from = ids.find(edge.from), to = ids.find(edge.to);
    if (from == ids.end() || to == ids.end() ||
        !edges.emplace(edge.from, edge.to, edge.type()).second) {
      return invalid("Media graph contains an unknown endpoint, untyped or duplicate connection");
    }
    const auto a = from->second, b = to->second;
    if (a == port_index || b == port_index) {
      if ((publishing && ((b == port_index && edge.type() != encoded) ||
                          (a == port_index && edge.type() != feedback))) ||
          (!publishing && ((a == port_index && edge.type() != encoded) ||
                           (b == port_index && edge.type() != keyframe)))) {
        return invalid("Media graph Room port has an invalid event direction or condition");
      }
      media_port |= edge.type() == encoded;
      feedback_port |= edge.type() == feedback;
    }
    neighbors[a].push_back(b);
    neighbors[b].push_back(a);
    if (edge.type() != feedback && edge.type() != keyframe) {
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
  frame.decode_completion = value.decode_completion;
  return frame;
}
}  // namespace

// The only transport bridge: Room handles, never legacy Endpoint/RoomEngine.
class MediaPort final : public livekit::sdk::rtc::TrackNode {
 public:
  explicit MediaPort(LocalTrack track) : TrackNode("room.port"), local(std::move(track)) {}

  MediaPort() : TrackNode("room.port") {}

  LocalTrack local;
  std::atomic<bool> keyframe{false};
  std::atomic<uint64_t> frames{0}, bytes{0};

  void receive(EncodedFrame value) {
    ++frames;
    bytes += value.buffer.size();
    send(rtc::nodes::EncodedFrameEvent(frame_copy<rtc::EncodedFrame>(value)));
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
    send(rtc::nodes::SenderFeedbackEvent(std::move(value)));
  }

 private:
  void bind(graphflow::Binder& ports) override {
    if (local) {
      output<rtc::nodes::SenderFeedbackEvent>(ports);
    } else {
      output<rtc::nodes::EncodedFrameEvent>(ports);
    }
    if (local) {
      ports.input(
          livekit::sdk::rtc::TrackPort<rtc::nodes::EncodedFrameEvent>::in,
          [this](const auto& event) {
            if (local.try_write(frame_copy<EncodedFrame>(event.frame)) == WriteStatus::accepted) {
              ++frames;
              bytes += event.frame.buffer.size();
            }
          });
    } else {
      ports.input(livekit::sdk::rtc::TrackPort<rtc::nodes::KeyframeRequestEvent>::in,
                  [this](const auto&) { keyframe = true; });
    }
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

void MediaPipeline::outputs(const std::shared_ptr<livekit::sdk::rtc::TrackNode>& source,
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
  // A delayed application preview must not reject a simultaneous recorder input.
  edges_.back().options = {graphflow::DeliveryPolicy::Latest, 2};
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
    context.port = port_->name();
    context.input = input_ ? input_->name() : "";
    context.codec = video_ ? video_->name() : audio_ ? audio_->name() : "";
    context.output = output_ ? output_->name() : "";
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
    context.node = node->name();
    node->set_failure_handler([weak = weak_from_this(), context](const auto& error) {
      if (auto self = weak.lock()) {
        self->fail(with_context(error, context));
      }
    });
  }
  // This is also the Room pump thread. Prepare and start acknowledge the owner
  // synchronously, without moving graph lifecycle operations between callers.
  std::promise<Result<void>> initialization;
  auto prepared = initialization.get_future();
  pump_ = std::thread([this, initialization = std::move(initialization)]() mutable {
    auto result = initialize_on_owner();
    const bool ready = bool(result);
    initialization.set_value(std::move(result));
    if (ready) {
      std::unique_lock lock(control_mutex_);
      control_changed_.wait(lock, [this] { return start_requested_ || stopping_; });
      const bool start = start_requested_;
      lock.unlock();
      if (start) {
        auto activated = activate_on_owner();
        const bool run = bool(activated);
        activation_.set_value(std::move(activated));
        if (run) {
          pump_on_owner();
        }
      }
    }
    stopping_ = true;
    stop_nodes();
    graph_.reset();
  });
  return prepared.get();
}

Result<void> MediaPipeline::initialize_on_owner() {
  graph_ = std::make_unique<graphflow::Graph>();
  rtc::detail::CallbackScope callback_scope;
  auto wired = rtc::wire_track_graph(*graph_, nodes_, edges_);
  if (!wired) {
    return Result<void>::failure(
        make_failure(ErrorDomain::media, ErrorCode::invalid_argument, wired.error.message));
  }
  auto prepared = graph_->start(graphflow::StartMode::PrepareOnly);
  if (!prepared) {
    auto error = snapshot().failure;
    return Result<void>::failure(error ? error
                                       : make_failure(ErrorDomain::media,
                                                      ErrorCode::processing_failed,
                                                      prepared.error.message));
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
  std::shared_ptr<livekit::sdk::rtc::TrackNode> raw;
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
  std::unique_lock lock(control_mutex_);
  if (start_requested_ || stopping_) {
    return Result<void>::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_state,
                                              "Media pipeline already started or closed"));
  }
  auto result = activation_.get_future();
  start_requested_ = true;
  lock.unlock();
  control_changed_.notify_one();
  return result.get();
}

Result<void> MediaPipeline::activate_on_owner() {
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
  return Result<void>::success();
}

void MediaPipeline::pump_on_owner() {
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
      if (port_->keyframe && SteadyClock::now() - last_keyframe > std::chrono::milliseconds(500)) {
        port_->keyframe = false;
        remote.request_keyframe();
        last_keyframe = SteadyClock::now();
      }
    }
  }
}

void MediaPipeline::stop_nodes() {
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
  // Device workers and graph mailboxes have separate queues. Deliver admitted
  // events before closing a receiver, in particular before finalizing a recording.
  auto settle = [&] {
    if (graph_) {
      auto view = graph_->snapshot();
      if (view && view->state == graphflow::GraphState::Running) {
        if (auto status = graph_->drain(); !status) {
          fail(
              make_failure(ErrorDomain::media, ErrorCode::processing_failed, status.error.message));
        }
      }
    }
  };
  settle();
  for (const auto& node : nodes_) {
    if (auto video = std::dynamic_pointer_cast<rtc::nodes::VideoCodecNode>(node)) {
      video->quiesce();
    }
    if (auto audio = std::dynamic_pointer_cast<rtc::nodes::AudioCodecNode>(node)) {
      audio->quiesce();
    }
  }
  settle();
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
    if (auto closed = graph_->close(); !closed) {
      fail(make_failure(ErrorDomain::media, ErrorCode::processing_failed, closed.error.message));
    }
  }
}

void MediaPipeline::close() {
  std::lock_guard close_lock(close_mutex_);
  {
    std::lock_guard lock(control_mutex_);
    stopping_ = true;
  }
  control_changed_.notify_one();
  if (pump_.joinable()) {
    pump_.join();
  }
}

MediaTrackSnapshot MediaPipeline::snapshot() const {
  MediaTrackSnapshot value;
  value.transport = publishing_ ? local.snapshot() : remote.snapshot();
  value.frames = port_ ? port_->frames.load() : 0;
  value.bytes = port_ ? port_->bytes.load() : 0;
  auto row = [&](const auto& node, const char* kind, uint64_t in, uint64_t out, uint64_t dropped) {
    value.nodes.push_back({node->name(), kind, in, out, dropped + node->delivery_drops(), 0});
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
                     [&](const auto& item) { return item.name == node->name(); })) {
      row(node, "TrackNode", 0, 0, 0);
      value.nodes.back().errors = node->last_failure() ? 1 : 0;
    }
  }
  for (const auto& edge : edges_) {
    value.edges.push_back({edge.from, edge.to, edge.type()});
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
