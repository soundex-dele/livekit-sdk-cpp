#include "studio_graph.hpp"
#include "livekit/player.hpp"
#include "engine/graph_engine.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <set>

namespace livekit::sdk::studio_detail {
namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using graphflow::core::LifecycleState;
using graphflow::engine::NodeConnection;

std::vector<float> take(std::deque<float>& queue, size_t n = 960) {
  std::vector<float> samples(n);
  for (size_t i = 0; i < n && !queue.empty(); ++i) {
    samples[i] = queue.front();
    queue.pop_front();
  }
  return samples;
}

void append(std::deque<float>& queue, const std::vector<float>& samples) {
  queue.insert(queue.end(), samples.begin(), samples.end());
  while (queue.size() > 19200)
    queue.pop_front();
}

float peak(const std::vector<float>& samples) {
  float result = 0;
  for (float sample : samples)
    result = std::max(result, std::abs(sample));
  return result;
}

void clamp(std::vector<float>& samples) {
  for (auto& sample : samples)
    sample = std::clamp(sample, -1.f, 1.f);
}

struct TickData {
  StudioScene scene;
  StudioAudioSettings audio;
  std::map<std::string, std::shared_ptr<const StudioImage>> remote;
  bool recording = false, video = false;
  std::filesystem::path path;
  Time now;
  std::vector<Time> audio_times;
};

struct Tick : graphflow::core::EventT<Tick> {
  std::shared_ptr<const TickData> data;
};

struct AudioInputData {
  struct Media {
    std::vector<float> samples;
    float gain;
  };

  Time captured;
  StudioAudioSettings settings;
  std::vector<float> microphone, system, remote;
  std::vector<Media> media;
};

struct AudioInput : graphflow::core::EventT<AudioInput> {
  std::shared_ptr<const AudioInputData> data;
};

struct AudioData {
  Time captured;
  bool monitor = false;
  std::vector<float> local, program, remote;
};

struct MixedAudio : graphflow::core::EventT<MixedAudio> {
  std::shared_ptr<const AudioData> data;
};

struct LayerData {
  Layers local, program;
  std::shared_ptr<const TickData> tick;
};

struct VideoLayers : graphflow::core::EventT<VideoLayers> {
  std::shared_ptr<const LayerData> data;
};

struct VideoData {
  std::shared_ptr<const StudioImage> local, program;
  Time captured;
};

struct ComposedVideo : graphflow::core::EventT<ComposedVideo> {
  std::shared_ptr<const VideoData> data;
};

struct Context {
  StudioGraphState& shared;
  std::exception_ptr failure;
  std::map<std::string, std::string> source_errors;
  std::string monitor_error;
};

class Node : public graphflow::core::GraphNode {
 public:
  Node(const char* name, Context& context) : GraphNode(name, nullptr), context_(context) {}

  uint64_t received = 0, published = 0;

 protected:
  Context& context_;

  StudioGraphState& shared() {
    return context_.shared;
  }

  template <class Event, class Callback>
  void listen(Callback callback) {
    const auto type = graphflow::core::detail::prettyName<Event>();
    // Nodes such as Recorder consume more than one type. Select only matching
    // edges; GraphNode::subscribe itself intentionally rejects mixed edge types.
    for (const auto& edge : getSubEdges())
      if (edge.dataType() == type) {
        edge.eventBus()->subscribe<Event>(edge.name(), [this, callback](Event& event) {
          if (state() != LifecycleState::Running || context_.failure)
            return;
          ++received;
          try {
            callback(event);
          } catch (...) {
            context_.failure = std::current_exception();
          }
        });
      }
  }

  template <class Event>
  void send(const Event& event) {
    if (context_.failure)
      return;
    ++published;
    // GraphNode::publish selects by condition, so the condition also carries the
    // type to keep a source's audio and video outputs on their respective edges.
    publish(event, graphflow::core::detail::prettyName<Event>());
  }
};

class ClockNode final : public Node {
 public:
  explicit ClockNode(Context& context) : Node("studio.clock", context) {}

  void tick(Time now) {
    auto data = std::make_shared<TickData>();
    data->now = now;
    {
      auto& s = shared();
      std::lock_guard lock(s.mutex);
      data->scene = s.scene;
      data->audio = s.settings;
      data->recording = s.recordWanted;
      data->path = s.recordPath;
      std::erase_if(s.remoteSeen, [&](const auto& entry) {
        if (now - entry.second < std::chrono::seconds(5))
          return false;
        s.remoteVideo.erase(entry.first);
        s.remoteAudio.erase(entry.first);
        return true;
      });
      data->remote = s.remoteVideo;
    }
    // Limit catch-up after a stalled source; never flood downstream queues.
    for (int i = 0; now >= next_audio_ && i < 10; ++i) {
      data->audio_times.push_back(next_audio_);
      next_audio_ += std::chrono::milliseconds(10);
    }
    if (now - next_audio_ > std::chrono::milliseconds(100))
      next_audio_ = now;
    if (now >= next_video_) {
      data->video = true;
      next_video_ += video_step_;
      if (now - next_video_ > video_step_) {
        std::lock_guard lock(shared().mutex);
        shared().status.dropped += uint64_t((now - next_video_) / video_step_);
        next_video_ = now + video_step_;
      }
    }
    Tick event;
    event.data = std::move(data);
    send(event);
  }

 private:
  Time next_audio_, next_video_;
  std::chrono::microseconds video_step_{};

  bool doStart() override {
    next_audio_ = next_video_ = Clock::now();
    video_step_ = std::chrono::microseconds(1000000 / shared().options.fps);
    return true;
  }
};

class SourcesNode final : public Node {
 public:
  explicit SourcesNode(Context& context) : Node("studio.sources", context) {}

 private:
  struct Source {
    StudioSource spec;
    std::unique_ptr<Capture> capture;
    std::unique_ptr<Player> player;
    std::shared_ptr<const StudioImage> image;
    std::deque<float> audio;
    std::string error;
    Time attempt = Clock::now();
  };

  std::map<std::string, Source> sources_;
  std::unique_ptr<AudioCapture> microphone_, system_;
  Time device_retry_{};

  bool doInit() override {
    listen<Tick>([this](const Tick& event) { process(event.data); });
    return true;
  }

  void doStop() override {
    sources_.clear();
    microphone_.reset();
    system_.reset();
    device_retry_ = {};
  }

  void doRelease() override {
    doStop();
  }

  void create(Source& source, const StudioSource& spec) {
    if (spec.kind == StudioSourceKind::camera || spec.kind == StudioSourceKind::screen ||
        spec.kind == StudioSourceKind::window)
      source.capture = std::make_unique<Capture>(spec);
    else if (spec.kind == StudioSourceKind::image)
      source.image = load_image(spec.input);
    else if (spec.kind == StudioSourceKind::text)
      source.image = text_image(spec.input, spec.color);
    else if (spec.kind == StudioSourceKind::video) {
      source.player = std::make_unique<Player>();
      source.player->open(spec.input).get();
      source.player->play().get();
    } else {
      auto image = std::make_shared<StudioImage>();
      image->width = image->height = 2;
      image->rgba.resize(16);
      for (int p = 0; p < 4; ++p)
        for (int c = 0; c < 4; ++c)
          image->rgba[p * 4 + c] = uint8_t(spec.color[c] * 255);
      source.image = std::move(image);
    }
  }

  void update(const TickData& tick) {
    auto& errors = context_.source_errors;
    errors.clear();
    if (tick.now >= device_retry_) {
      device_retry_ = tick.now + std::chrono::seconds(5);
      if (microphone_ && !microphone_->error().empty())
        microphone_.reset();
      if (system_ && !system_->error().empty())
        system_.reset();
    }
    if (tick.audio.microphone && !microphone_)
      microphone_ = std::make_unique<AudioCapture>(false);
    if (!tick.audio.microphone)
      microphone_.reset();
    if (tick.audio.system && !system_)
      system_ = std::make_unique<AudioCapture>(true);
    if (!tick.audio.system)
      system_.reset();
    std::set<std::string> active;
    for (const auto& spec : tick.scene.sources) {
      if (spec.kind == StudioSourceKind::remote || !spec.visible)
        continue;
      active.insert(spec.id);
      auto previous = sources_.find(spec.id);
      if (previous != sources_.end()) {
        const auto& old = previous->second;
        if (old.spec.kind != spec.kind || old.spec.input != spec.input ||
            old.spec.color != spec.color ||
            (!old.error.empty() && tick.now - old.attempt > std::chrono::seconds(5)))
          sources_.erase(previous);
      }
      auto [it, inserted] = sources_.try_emplace(spec.id);
      auto& source = it->second;
      auto previous_error = source.error;
      source.spec = spec;
      if (inserted)
        try {
          create(source, spec);
        } catch (const std::exception& e) {
          source.error = e.what();
        }
      if (source.capture) {
        source.error = source.capture->error();
        if (source.error.empty())
          source.image = source.capture->frame();
      }
      if (source.player) {
        while (auto frame = source.player->poll_video()) {
          auto image = std::make_shared<StudioImage>();
          image->width = frame->width;
          image->height = frame->height;
          image->rgba.resize(size_t(image->width) * image->height * 4);
          for (int y = 0; y < image->height; ++y)
            memcpy(image->rgba.data() + size_t(y) * image->width * 4,
                   frame->rgba->data() + size_t(y) * frame->stride, size_t(image->width) * 4);
          source.image = std::move(image);
        }
        while (auto frame = source.player->poll_audio())
          append(source.audio, *frame->pcm);
        auto state = source.player->snapshot();
        if (state.state == PlayerState::ended && spec.loop) {
          source.player->seek(std::chrono::microseconds(0)).get();
          source.player->play().get();
        }
        if (!state.error.empty())
          source.error = state.error;
      }
      if (!source.error.empty()) {
        if (source.error != previous_error)
          source.image.reset();
        errors[spec.id] = source.error;
        if (!source.image)
          source.image = text_image("素材不可用 · 正在重试", {.7f, .72f, .78f, 1});
      }
    }
    std::erase_if(sources_, [&](const auto& entry) { return !active.contains(entry.first); });
    if (microphone_ && !microphone_->error().empty())
      errors["microphone"] = microphone_->error();
    if (system_ && !system_->error().empty())
      errors["system"] = system_->error();
  }

  void process(const std::shared_ptr<const TickData>& tick) {
    update(*tick);
    for (auto time : tick->audio_times) {
      auto audio = std::make_shared<AudioInputData>();
      audio->captured = time;
      audio->settings = tick->audio;
      audio->microphone = microphone_ ? microphone_->take(480) : std::vector<float>(960);
      audio->system = system_ ? system_->take(480) : std::vector<float>(960);
      for (auto& [id, source] : sources_) {
        auto samples = take(source.audio);
        if (!source.spec.muted)
          audio->media.push_back(
              {std::move(samples), tick->audio.media_volume * source.spec.volume});
      }
      audio->remote.resize(960);
      {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        for (auto& [id, queue] : s.remoteAudio)
          studio_mix(audio->remote, take(queue), tick->audio.remote_volume);
      }
      AudioInput event;
      event.data = std::move(audio);
      send(event);
    }
    if (!tick->video)
      return;
    auto layers = std::make_shared<LayerData>();
    layers->tick = tick;
    for (const auto& spec : tick->scene.sources) {
      std::shared_ptr<const StudioImage> image;
      if (spec.kind == StudioSourceKind::remote) {
        auto it = tick->remote.find(spec.input);
        if (it != tick->remote.end())
          image = it->second;
      } else {
        auto it = sources_.find(spec.id);
        if (it != sources_.end())
          image = it->second.image;
      }
      if (image) {
        layers->program.emplace_back(spec, image);
        if (spec.kind != StudioSourceKind::remote)
          layers->local.emplace_back(spec, image);
      }
    }
    VideoLayers event;
    event.data = std::move(layers);
    send(event);
  }
};

class MixerNode final : public Node {
 public:
  explicit MixerNode(Context& context) : Node("studio.mixer", context) {}

 private:
  bool doInit() override {
    listen<AudioInput>([this](const AudioInput& event) {
      const auto& in = *event.data;
      auto out = std::make_shared<AudioData>();
      out->captured = in.captured;
      out->monitor = in.settings.monitor;
      out->local.resize(960);
      studio_mix(out->local, in.microphone, in.settings.microphone_volume);
      studio_mix(out->local, in.system, in.settings.system_volume);
      for (const auto& media : in.media)
        studio_mix(out->local, media.samples, media.gain);
      clamp(out->local);
      out->program = out->local;
      out->remote = in.remote;
      studio_mix(out->program, out->remote, 1);
      clamp(out->program);
      {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.status.microphone_peak = peak(in.microphone) * in.settings.microphone_volume;
        s.status.system_peak = peak(in.system) * in.settings.system_volume;
        s.status.output_peak = peak(out->program);
      }
      MixedAudio result;
      result.data = std::move(out);
      send(result);
    });
    return true;
  }
};

class CompositeNode final : public Node {
 public:
  explicit CompositeNode(Context& context) : Node("studio.compositor", context) {}

 private:
  std::unique_ptr<Compositor> compositor_;

  bool doInit() override {
    const auto& o = shared().options;
    compositor_ = std::make_unique<Compositor>(o.width, o.height, o.software_gpu);
    listen<VideoLayers>([this](const VideoLayers& event) {
      const auto& in = *event.data;
      const auto& tick = *in.tick;
      auto out = std::make_shared<VideoData>();
      out->captured = tick.now;
      out->local = std::make_shared<StudioImage>(compositor_->render(in.local));
      auto program = in.program;
      bool explicit_remote =
          std::any_of(tick.scene.sources.begin(), tick.scene.sources.end(),
                      [](const auto& s) { return s.kind == StudioSourceKind::remote; });
      if (!tick.remote.empty() && !explicit_remote) {
        // Only the program receives return feeds. The interaction room publishes
        // the full-canvas local image, so PK cannot recursively composite itself.
        program.clear();
        StudioSource base;
        base.id = "program-local";
        base.transform = {0, 0, .5f, 1};
        program.emplace_back(base, out->local);
        int i = 0;
        for (const auto& [id, image] : tick.remote) {
          StudioSource tile;
          tile.id = id;
          tile.transform = {.5f, float(i) / float(tick.remote.size()), .5f,
                            1.f / float(tick.remote.size())};
          program.emplace_back(tile, image);
          ++i;
        }
      }
      out->program = tick.remote.empty() && in.local.size() == program.size()
                         ? out->local
                         : std::make_shared<StudioImage>(compositor_->render(program));
      ComposedVideo result;
      result.data = std::move(out);
      send(result);
    });
    return true;
  }

  void doRelease() override {
    compositor_.reset();
  }
};

class VideoOutputNode final : public Node {
 public:
  explicit VideoOutputNode(Context& context) : Node("studio.video-output", context) {}

 private:
  bool doInit() override {
    listen<ComposedVideo>([this](const ComposedVideo& event) {
      const auto& frame = *event.data;
      auto video = to_i420(*frame.program);
      auto& s = shared();
      auto result = s.programVideo->write(video);
      s.localVideo->write(frame.program == frame.local ? video : to_i420(*frame.local));
      if (result == VideoWriteStatus::backpressure && s.programVideo->stats().attached) {
        std::lock_guard lock(s.mutex);
        ++s.status.dropped;
      }
    });
    return true;
  }

  void doRelease() override {
    shared().programVideo->close();
    shared().localVideo->close();
  }
};

class AudioOutputNode final : public Node {
 public:
  explicit AudioOutputNode(Context& context) : Node("studio.audio-output", context) {}

 private:
  bool doInit() override {
    listen<MixedAudio>([this](const MixedAudio& event) {
      const auto& audio = *event.data;
      PcmInputFrame frame;
      frame.capture_time = audio.captured;
      frame.float_samples = audio.local;
      shared().localAudio->write(frame);
      frame.float_samples = audio.program;
      shared().programAudio->write(frame);
    });
    return true;
  }

  void doRelease() override {
    shared().programAudio->close();
    shared().localAudio->close();
  }
};

class PreviewNode final : public Node {
 public:
  explicit PreviewNode(Context& context) : Node("studio.preview", context) {}

 private:
  bool doInit() override {
    listen<ComposedVideo>([this](const ComposedVideo& event) {
      auto& s = shared();
      std::lock_guard lock(s.mutex);
      s.status.preview = event.data->program;
      ++s.status.frames;
      s.status.source_errors = context_.source_errors;
      if (!context_.monitor_error.empty())
        s.status.source_errors["monitor"] = context_.monitor_error;
    });
    return true;
  }
};

class MonitorNode final : public Node {
 public:
  explicit MonitorNode(Context& context) : Node("studio.monitor", context) {}

 private:
  std::unique_ptr<Monitor> monitor_;
  Time retry_{};

  bool doInit() override {
    listen<MixedAudio>([this](const MixedAudio& event) {
      const auto& audio = *event.data;
      if (!audio.monitor) {
        monitor_.reset();
        context_.monitor_error.clear();
        return;
      }
      if (audio.captured >= retry_) {
        retry_ = audio.captured + std::chrono::seconds(5);
        if (monitor_ && !monitor_->error().empty())
          monitor_.reset();
      }
      if (!monitor_)
        monitor_ = std::make_unique<Monitor>();
      monitor_->write(audio.remote);
      context_.monitor_error = monitor_->error();
    });
    return true;
  }

  void doStop() override {
    monitor_.reset();
    retry_ = {};
    context_.monitor_error.clear();
  }

  void doRelease() override {
    doStop();
  }
};

class RecorderNode final : public Node {
 public:
  explicit RecorderNode(Context& context) : Node("studio.recorder", context) {}

 private:
  std::unique_ptr<Recorder> recorder_;

  void error(const std::exception& e) {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.status.error = e.what();
    s.recordWanted = false;
  }

  void finish() {
    if (recorder_)
      try {
        recorder_->finish();
      } catch (const std::exception& e) {
        error(e);
      }
    recorder_.reset();
    std::lock_guard lock(shared().mutex);
    shared().status.recording = false;
  }

  bool doInit() override {
    listen<Tick>([this](const Tick& event) {
      const auto& tick = *event.data;
      if (!tick.recording && recorder_)
        finish();
      if (tick.recording && !recorder_)
        try {
          auto& s = shared();
          const auto& o = s.options;
          recorder_ = std::make_unique<Recorder>(tick.path, o.width, o.height, o.fps);
          std::lock_guard lock(s.mutex);
          s.status.recording = true;
          s.status.recording_path = tick.path.string();
        } catch (const std::exception& e) {
          error(e);
        }
    });
    listen<MixedAudio>([this](const MixedAudio& event) {
      if (recorder_)
        try {
          recorder_->audio(event.data->program, event.data->captured);
        } catch (const std::exception& e) {
          error(e);
        }
    });
    listen<ComposedVideo>([this](const ComposedVideo& event) {
      if (recorder_)
        try {
          recorder_->video(*event.data->program, event.data->captured);
        } catch (const std::exception& e) {
          error(e);
        }
    });
    return true;
  }

  void doStop() override {
    finish();
  }

  void doRelease() override {
    finish();
  }
};

template <class Event>
NodeConnection link(const std::shared_ptr<Node>& from, const std::shared_ptr<Node>& to) {
  auto type = graphflow::core::detail::prettyName<Event>();
  return {from->getName(), to->getName(), type, type};
}
}  // namespace

StudioGraphState::StudioGraphState(StudioOptions o) : options(o) {
  if ((o.width != 1280 || o.height != 720) && (o.width != 720 || o.height != 1280) &&
      (o.width != 1920 || o.height != 1080) && (o.width != 1080 || o.height != 1920))
    throw std::invalid_argument("请选择 720p 或 1080p 横竖屏");
  if (o.fps != 30)
    throw std::invalid_argument("首版帧率为 30 fps");
  I420SourceOptions video;
  video.width = o.width;
  video.height = o.height;
  programVideo = std::make_shared<I420VideoSource>(video);
  localVideo = std::make_shared<I420VideoSource>(video);
  PcmSourceOptions audio;
  audio.channels = 2;
  audio.max_queue_duration = std::chrono::milliseconds(60);
  audio.format = AudioSampleFormat::float32;
  programAudio = std::make_shared<PcmAudioSource>(audio);
  localAudio = std::make_shared<PcmAudioSource>(audio);
}

struct StudioGraph::Impl {
  Context context;
  graphflow::engine::GraphEngine engine{1};
  std::vector<std::shared_ptr<Node>> nodes;
  std::shared_ptr<ClockNode> clock;

  explicit Impl(StudioGraphState& shared) : context{shared} {
    clock = std::make_shared<ClockNode>(context);
    auto sources = std::make_shared<SourcesNode>(context);
    auto mixer = std::make_shared<MixerNode>(context);
    auto compositor = std::make_shared<CompositeNode>(context);
    auto video = std::make_shared<VideoOutputNode>(context);
    auto audio = std::make_shared<AudioOutputNode>(context);
    auto preview = std::make_shared<PreviewNode>(context);
    auto monitor = std::make_shared<MonitorNode>(context);
    auto recorder = std::make_shared<RecorderNode>(context);
    // Downstream subscriptions/resources are ready before the clock starts.
    // No producer runs during stop/release: step() is synchronous on this owner.
    nodes = {recorder, monitor, preview, video, audio, compositor, mixer, sources, clock};
    std::vector<std::shared_ptr<graphflow::core::GraphNode>> lifecycle(nodes.begin(), nodes.end());
    std::vector<NodeConnection> edges{
        link<Tick>(clock, recorder),  // Apply recording control before this tick's media.
        link<Tick>(clock, sources),
        link<AudioInput>(sources, mixer),
        link<VideoLayers>(sources, compositor),
        link<MixedAudio>(mixer, audio),
        link<MixedAudio>(mixer, monitor),
        link<MixedAudio>(mixer, recorder),
        link<ComposedVideo>(compositor, video),
        link<ComposedVideo>(compositor, preview),
        link<ComposedVideo>(compositor, recorder)};
    if (!engine.loadFromNodes(lifecycle, edges))
      throw std::runtime_error("studio/build: graph load failed");
  }

  ~Impl() {
    engine.release();
  }
};

StudioGraph::StudioGraph(StudioGraphState& state) : impl_(std::make_unique<Impl>(state)) {}

StudioGraph::~StudioGraph() = default;

void StudioGraph::start() {
  if (impl_->context.failure)
    std::rethrow_exception(impl_->context.failure);
  if (!impl_->engine.init() || !impl_->engine.start())
    throw std::runtime_error("studio/start: graph is not ready");
}

void StudioGraph::step(Time now) {
  if (impl_->engine.state() != LifecycleState::Running)
    throw std::logic_error("studio/step: graph is not running");
  if (impl_->context.failure)
    std::rethrow_exception(impl_->context.failure);
  impl_->clock->tick(now);
  // EventBus logs and swallows callback exceptions. Keep a shared failure slot
  // so processing errors reach StudioSnapshot rather than silently losing media.
  if (impl_->context.failure)
    std::rethrow_exception(impl_->context.failure);
}

void StudioGraph::stop() {
  impl_->engine.stop();
}

void StudioGraph::release() {
  impl_->engine.release();
}

std::vector<StudioGraph::NodeInfo> StudioGraph::nodes() const {
  std::vector<NodeInfo> result;
  for (const auto& node : impl_->nodes)
    result.push_back({node->getName(), node->state(), node->received, node->published});
  return result;
}

std::vector<StudioGraph::Connection> StudioGraph::connections() const {
  std::vector<Connection> result;
  for (const auto& node : impl_->nodes)
    for (const auto& edge : node->getPubEdges())
      result.push_back({edge.from(), edge.to(), edge.dataType()});
  return result;
}
}  // namespace livekit::sdk::studio_detail
