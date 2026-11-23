#include "livekit/compat/algorithm.hpp"
#include "studio_graph.hpp"
#include "media_nodes/capture/audio_source.hpp"
#include "recording_session.hpp"
#include "livekit/player.hpp"
#include <graphflow/graph.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <set>
#include <condition_variable>
#include <thread>
#include <utility>

namespace livekit::sdk::studio_detail {
namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
namespace gf = graphflow;

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
  std::map<std::string, std::shared_ptr<const StudioImage>> remote;
  bool video = false;
  Time now;

};

struct Tick {
  static constexpr const char* port = "Tick";
  std::shared_ptr<const TickData> data;
};

struct AudioInputData {
  struct Media {
    std::vector<float> samples;
    float gain;
  };

  Time captured;
  StudioAudioSettings settings;
  size_t remoteFloatsRead = 0;  // Diagnostic only: excludes padding for missing input.
  std::vector<float> microphone, system, remote;
  std::vector<Media> media;
};

struct AudioInput {
  static constexpr const char* port = "AudioInput";
  std::shared_ptr<const AudioInputData> data;
};

struct AudioData {
  Time captured, mixed;
  bool monitor = false;
  std::vector<float> local, program, remote;
};

struct MixedAudio {
  static constexpr const char* port = "MixedAudio";
  std::shared_ptr<const AudioData> data;
};

struct RecordingAudio {
  static constexpr const char* port = "RecordingAudio";
  std::shared_ptr<const AudioData> data;
};

struct LayerData {
  Layers local, program;
  std::shared_ptr<const TickData> tick;
};

struct VideoLayers {
  static constexpr const char* port = "VideoLayers";
  std::shared_ptr<const LayerData> data;
};

struct VideoData {
  std::shared_ptr<const StudioImage> local, program;
  Time captured;
};

struct ComposedVideo {
  static constexpr const char* port = "ComposedVideo";
  std::shared_ptr<const VideoData> data;
};

struct RecordingVideo {
  static constexpr const char* port = "RecordingVideo";
  std::shared_ptr<const VideoData> data;
};

struct PlaybackState {
  std::mutex mutex;
  std::optional<media::VideoFrame> video;
  std::deque<float> audio;
  std::deque<std::function<void()>> completions;
  PlayerSnapshot snapshot;
  uint64_t generation = 0;
  std::string error;
  bool pending = true;

  void accept(uint64_t value) {
    if (generation != value) {
      generation = value;
      video.reset();
      audio.clear();
    }
  }
};

struct AudioTick {
  static constexpr const char* port = "AudioTick";
  Time captured;
};

struct Context {
  StudioGraphState& shared;
  Failure failure;
  std::map<std::string, std::string> source_errors;
  // Playback registry and recording admission pointer are protected by shared.mutex.
  std::map<std::string, std::pair<std::string, std::weak_ptr<PlaybackState>>> playback;
  std::map<std::string, std::string> audio_errors;
  std::string monitor_error;
  RecordingSession* recording = nullptr;
  Time recording_origin{};
};

gf::Status graph_status(const Result<void>& result) {
  if (result) {
    return {};
  }
  return {{gf::ErrorCode::NodeFailure, result.error().message, result.failure_record()}};
}

Result<void> graph_error(const gf::Error& error) {
  if (error.detail) {
    // All application details in this private graph originate in graph_status().
    return failed(std::static_pointer_cast<const FailureInfo>(error.detail));
  }
  return failed("studio/graph: " + error.message);
}

template <class E>
Result<void> deliver(const gf::Emitter<E>& writer, const E& event, uint64_t& published, const std::string& label = {}) {
  ++published;
  auto result = writer.emit(event, label);
  if (result.status == gf::Admission::Accepted) {
    return Result<void>::success();
  }
  if (result.status == gf::Admission::Failed) {
    return graph_error(result.error);
  }
  return failed("studio/graph: output did not deliver " + std::string(E::port));
}

class Node : public gf::INode {
 public:
  Node(Context& context, uint64_t& published) : context_(context), published_(published) {}

 protected:
  Context& context_;
  uint64_t& published_;

  StudioGraphState& shared() {
    return context_.shared;
  }

  template <class E, class F>
  void listen(gf::Binder& ports, F handle) {
    ports.input(gf::In<E>{E::port, gf::Connections::ExactlyOne},
                [handle](const E& event) -> gf::Status {
                  if constexpr (std::is_void_v<std::invoke_result_t<F, const E&>>) {
                    handle(event);
                    return {};
                  } else {
                    return graph_status(handle(event));
                  }
                });
  }
};

// Video/control clock on the compositor owner. It never generates PCM slots.
class ClockSource {
 public:
  explicit ClockSource(StudioGraphState& state) : state_(state) {}

  Tick tick(Time now) {
    auto data = std::make_shared<TickData>();
    data->now = now;
    {
      auto& s = state_;
      std::lock_guard lock(s.mutex);
      data->scene = s.scene;
      livekit::compat::erase_if(s.remoteSeen, [&](const auto& entry) {
        if (now - entry.second < std::chrono::seconds(5))
          return false;
        s.remoteVideo.erase(entry.first);
        s.remoteAudio.erase(entry.first);
        return true;
      });
      data->remote = s.remoteVideo;
    }
    if (now >= next_video_) {
      data->video = true;
      next_video_ += video_step_;
      if (now - next_video_ > video_step_) {
        std::lock_guard lock(state_.mutex);
        // next_video_ itself is also due and is discarded by the reschedule.
        state_.status.dropped += 1 + uint64_t((now - next_video_) / video_step_);
        next_video_ = now + video_step_;
      }
    }
    Tick event;
    event.data = std::move(data);
    return event;
  }

  void reset() {
    next_video_ = Clock::now();
    video_step_ = std::chrono::microseconds(1000000 / state_.options.fps);
  }

 private:
  StudioGraphState& state_;
  Time next_video_;
  std::chrono::microseconds video_step_{};
};

class SourcesNode final : public Node {
 public:
  using Node::Node;

 private:
  // Callbacks retain no scene/node pointer. Keep only the newest image and at
  // most 200 ms of stereo PCM; a slow compositor cannot grow player output.
  struct Source {
    StudioSource spec;
    std::unique_ptr<Capture> capture;
    std::shared_ptr<PlaybackState> playback;
    std::shared_ptr<Player> player;
    std::shared_ptr<const StudioImage> image;

    std::string error;
    Time attempt = Clock::now();
  };

  std::map<std::string, Source> sources_;


  static auto dispatcher(const std::shared_ptr<PlaybackState>& state) {
    return [weak = std::weak_ptr(state)](std::function<void()> task) -> Result<void> {
      auto target = weak.lock();
      if (!target) {
        return failed("Studio source removed", ErrorCode::cancelled);
      }
      std::lock_guard lock(target->mutex);
      if (target->completions.size() >= 8) {
        return failed("Studio source completion queue exhausted", ErrorCode::resource_exhausted);
      }
      target->completions.push_back(std::move(task));
      return Result<void>::success();
    };
  }

  static void complete(Source& source, AsyncResult<void> operation) {
    std::move(operation).then(dispatcher(source.playback),
                              [weak = std::weak_ptr(source.playback)](AsyncResult<void> result) {
                                auto target = weak.lock();
                                if (!target) {
                                  return;
                                }
                                auto completed = result.get();
                                std::string error =
                                    completed ? std::string{} : completed.error().message;
                                std::lock_guard lock(target->mutex);
                                target->pending = false;
                                target->error = std::move(error);
                              });
  }


  gf::Emitter<VideoLayers> video_out_;

  void bind(gf::Binder& ports) override {

    video_out_ = ports.output(gf::Out<VideoLayers>{VideoLayers::port, gf::Connections::AtLeastOne});
    listen<Tick>(ports, [this](const Tick& event) { return process(event.data); });
  }

  gf::Status stop() override {
    sources_.clear();
    std::lock_guard lock(shared().mutex);
    context_.playback.clear();
    return {};
  }

  gf::Status release() override {
    return stop();
  }

  Result<void> create(Source& source, const StudioSource& spec) {
    if (spec.kind == StudioSourceKind::camera || spec.kind == StudioSourceKind::screen ||
        spec.kind == StudioSourceKind::window)
      source.capture = std::make_unique<Capture>(spec);
    else if (spec.kind == StudioSourceKind::image) {
      LK_STUDIO_ASSIGN(image, load_image(spec.input));
      source.image = std::move(image);
    } else if (spec.kind == StudioSourceKind::text) {
      LK_STUDIO_ASSIGN(image, text_image(spec.input, spec.color));
      source.image = std::move(image);
    } else if (spec.kind == StudioSourceKind::video) {
      source.playback = std::make_shared<PlaybackState>();
      source.player = std::make_shared<Player>();
      const auto weak = std::weak_ptr(source.playback);
      PlayerCallbacks callbacks;
      callbacks.video = [weak](const media::VideoFrame& frame) {
        if (auto target = weak.lock()) {
          std::lock_guard lock(target->mutex);
          target->accept(frame.generation);
          target->video = frame;
        }
        return Result<void>::success();
      };
      callbacks.audio = [weak](const media::AudioFrame& frame) {
        if (auto target = weak.lock()) {
          std::lock_guard lock(target->mutex);
          target->accept(frame.generation);
          append(target->audio, *frame.pcm);
        }
        return Result<void>::success();
      };
      callbacks.state = [weak](const PlayerSnapshot& snapshot) {
        if (auto target = weak.lock()) {
          std::lock_guard lock(target->mutex);
          target->accept(snapshot.generation);
          target->snapshot = snapshot;
        }
        return Result<void>::success();
      };
      LK_STUDIO_TRY(source.player->set_callbacks(std::move(callbacks)));
      complete(source, source.player->open(spec.input)
                           .then(dispatcher(source.playback),
                                 [player = std::weak_ptr(source.player)](
                                     AsyncResult<PlayerMediaInfo> result) -> AsyncResult<void> {
                                   auto opened = result.get();
                                   if (!opened) {
                                     CompletionSource<void> failed_source;
                                     auto ready = failed_source.get_result();
                                     failed_source.set_failure(opened.failure_record());
                                     return ready;
                                   }
                                   auto target = player.lock();
                                   if (!target) {
                                     CompletionSource<void> failed_source;
                                     auto ready = failed_source.get_result();
                                     failed_source.set_failure(
                                         make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled,
                                                      "Studio source removed"));
                                     return ready;
                                   }
                                   return target->play();
                                 }));
    } else {
      auto image = std::make_shared<StudioImage>();
      image->width = image->height = 2;
      image->rgba.resize(16);
      for (int p = 0; p < 4; ++p)
        for (int c = 0; c < 4; ++c)
          image->rgba[p * 4 + c] = uint8_t(spec.color[c] * 255);
      source.image = std::move(image);
    }
    return Result<void>::success();
  }

  void update(const TickData& tick) {
    auto& errors = context_.source_errors;
    errors.clear();
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
      if (inserted) {
        auto created = create(source, spec);
        if (!created)
          source.error = created.error().message;
      }
      if (source.capture) {
        source.error = source.capture->error();
        if (source.error.empty())
          source.image = source.capture->frame();
      }
      if (source.player) {
        std::deque<std::function<void()>> completions;
        {
          std::lock_guard lock(source.playback->mutex);
          completions.swap(source.playback->completions);
        }
        for (auto& completion : completions) {
          completion();
        }
        std::optional<media::VideoFrame> frame;
        bool replay = false;
        {
          auto& playback = *source.playback;
          std::lock_guard lock(playback.mutex);
          frame = std::exchange(playback.video, {});
          source.error = playback.error.empty() ? playback.snapshot.error : playback.error;
          replay = playback.snapshot.state == PlayerState::ended && spec.loop && !playback.pending;
          if (replay) {
            playback.pending = true;
            playback.snapshot.state = PlayerState::ready;
          }
        }
        if (frame) {
          auto image = std::make_shared<StudioImage>();
          image->width = frame->width;
          image->height = frame->height;
          image->rgba.resize(size_t(image->width) * image->height * 4);
          for (int y = 0; y < image->height; ++y) {
            memcpy(image->rgba.data() + size_t(y) * image->width * 4,
                   frame->rgba->data() + size_t(y) * frame->stride, size_t(image->width) * 4);
          }
          source.image = std::move(image);
        }
        if (replay) {
          // Player owns rewind-at-EOS; never block the compositor on file I/O.
          complete(source, source.player->play());
        }
      }
      if (!source.error.empty()) {
        if (source.error != previous_error)
          source.image.reset();
        errors[spec.id] = source.error;
        if (!source.image) {
          auto image = text_image("素材不可用 · 正在重试", {.7f, .72f, .78f, 1});
          if (image)
            source.image = std::move(*image);
          else
            errors[spec.id] += "; " + image.error().message;
        }
      }
    }
    livekit::compat::erase_if(sources_,
                              [&](const auto& entry) { return !active.count(entry.first); });
    std::lock_guard lock(shared().mutex);
    context_.playback.clear();
    for (const auto& [id, source] : sources_) {
      if (source.playback) {
        context_.playback.emplace(id, std::make_pair(source.spec.input, source.playback));
      }
    }
  }

  Result<void> process(const std::shared_ptr<const TickData>& tick) {
    update(*tick);
    if (!tick->video)
      return Result<void>::success();
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
    const auto& trace = shared().traceVideoDispatch;
    const auto enter = trace ? Clock::now() : Time{};
    auto status = deliver(video_out_, event, published_);
    if (trace) {
      trace(enter, Clock::now());
    }
    return status;
  }
};

class AudioSourcesNode final : public Node {
 public:
  using Node::Node;

 private:
  std::unique_ptr<AudioCapture> microphone_, system_;
  Time device_retry_{};
  gf::Emitter<AudioInput> output_;

  void bind(gf::Binder& ports) override {
    output_ = ports.output(gf::Out<AudioInput>{AudioInput::port, gf::Connections::ExactlyOne});
    listen<AudioTick>(ports, [this](const AudioTick& tick) { return collect(tick.captured); });
  }

  gf::Status stop() override {
    microphone_.reset();
    system_.reset();
    device_retry_ = {};
    std::lock_guard lock(shared().mutex);
    context_.audio_errors.clear();
    return {};
  }

  Result<void> collect(Time now) {
    auto audio = std::make_shared<AudioInputData>();
    audio->captured = now;
    audio->remote.resize(960);
    auto& settings = audio->settings;
    std::vector<std::pair<std::shared_ptr<PlaybackState>, float>> players;
    {
      auto& s = shared();
      std::lock_guard lock(s.mutex);
      settings = s.settings;
      for (auto& [id, queue] : s.remoteAudio) {
        if (s.traceMix) {
          audio->remoteFloatsRead += (std::min)(queue.size(), audio->remote.size());
        }
        LK_STUDIO_TRY(studio_mix(audio->remote, take(queue), settings.remote_volume));
      }
      for (const auto& spec : s.scene.sources) {
        auto it = context_.playback.find(spec.id);
        if (spec.visible && spec.kind == StudioSourceKind::video && it != context_.playback.end() &&
            it->second.first == spec.input) {
          if (auto src = it->second.second.lock()) {
            players.emplace_back(std::move(src),
                                 spec.muted ? 0.f : settings.media_volume * spec.volume);
          }
        }
      }
    }
    for (auto& [src, gain] : players) {
      std::lock_guard lock(src->mutex);
      audio->media.push_back({take(src->audio), gain});
    }
    if (now >= device_retry_) {
      device_retry_ = now + std::chrono::seconds(5);
      if (microphone_ && !microphone_->error().empty()) {
        microphone_.reset();
      }
      if (system_ && !system_->error().empty()) {
        system_.reset();
      }
    }
    if (settings.microphone && !microphone_) {
      microphone_ = std::make_unique<AudioCapture>(false);
    }
    if (!settings.microphone) {
      microphone_.reset();
    }
    if (settings.system && !system_) {
      system_ = std::make_unique<AudioCapture>(true);
    }
    if (!settings.system) {
      system_.reset();
    }
    audio->microphone = microphone_ ? microphone_->take(480) : std::vector<float>(960);
    audio->system = system_ ? system_->take(480) : std::vector<float>(960);
    {
      std::lock_guard lock(shared().mutex);
      auto& errors = context_.audio_errors;
      errors.clear();
      if (microphone_ && !microphone_->error().empty()) {
        errors["microphone"] = microphone_->error();
      }
      if (system_ && !system_->error().empty()) {
        errors["system"] = system_->error();
      }
    }
    return deliver(output_, AudioInput{std::move(audio)}, published_);
  }
};

class MixerNode final : public Node {
 public:
  using Node::Node;

 private:
  gf::Emitter<MixedAudio> mixed_out_;
  gf::Emitter<RecordingAudio> recording_out_;

  void bind(gf::Binder& ports) override {
    mixed_out_ = ports.output(gf::Out<MixedAudio>{MixedAudio::port, gf::Connections::AtLeastOne});
    recording_out_ =
        ports.output(gf::Out<RecordingAudio>{RecordingAudio::port, gf::Connections::AtLeastOne});
    listen<AudioInput>(ports, [this](const AudioInput& event) -> Result<void> {
      const auto& in = *event.data;
      auto out = std::make_shared<AudioData>();
      out->captured = in.captured;
      out->monitor = in.settings.monitor;
      out->local.resize(960);
      LK_STUDIO_TRY(studio_mix(out->local, in.microphone, in.settings.microphone_volume));
      LK_STUDIO_TRY(studio_mix(out->local, in.system, in.settings.system_volume));
      for (const auto& media : in.media)
        LK_STUDIO_TRY(studio_mix(out->local, media.samples, media.gain));
      clamp(out->local);
      out->program = out->local;
      out->remote = in.remote;
      LK_STUDIO_TRY(studio_mix(out->program, out->remote, 1));
      clamp(out->program);
      if (shared().traceMix) {
        shared().traceMix(in.captured, in.remoteFloatsRead, in.remote, out->program);
      }
      {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.status.microphone_peak = peak(in.microphone) * in.settings.microphone_volume;
        s.status.system_peak = peak(in.system) * in.settings.system_volume;
        s.status.output_peak = peak(out->program);
      }
      if (shared().traceProgramWrite) {
        out->mixed = Clock::now();
      }
      MixedAudio result;
      result.data = std::move(out);
      LK_STUDIO_TRY(deliver(mixed_out_, result, published_));
      RecordingAudio recording;
      recording.data = result.data;
      return deliver(recording_out_, recording, published_);
    });
  }
};

class CompositeNode final : public Node {
 public:
  using Node::Node;

 private:
  std::unique_ptr<Compositor> compositor_;

  gf::Status init() override {
    const auto& o = shared().options;
    compositor_ = std::make_unique<Compositor>(o.width, o.height, o.software_gpu);
    return graph_status(compositor_->status());
  }

  gf::Emitter<ComposedVideo> composed_out_;
  gf::Emitter<RecordingVideo> recording_out_;

  void bind(gf::Binder& ports) override {
    composed_out_ =
        ports.output(gf::Out<ComposedVideo>{ComposedVideo::port, gf::Connections::AtLeastOne});
    recording_out_ =
        ports.output(gf::Out<RecordingVideo>{RecordingVideo::port, gf::Connections::AtLeastOne});
    listen<VideoLayers>(ports, [this](const VideoLayers& event) -> Result<void> {
      const auto& in = *event.data;
      const auto& tick = *in.tick;
      auto out = std::make_shared<VideoData>();
      out->captured = tick.now;
      LK_STUDIO_ASSIGN(local, compositor_->render(in.local));
      out->local = std::make_shared<StudioImage>(std::move(local));
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
      out->program = out->local;
      if (!tick.remote.empty() || in.local.size() != program.size()) {
        LK_STUDIO_ASSIGN(rendered, compositor_->render(program));
        out->program = std::make_shared<StudioImage>(std::move(rendered));
      }
      ComposedVideo result;
      result.data = std::move(out);
      LK_STUDIO_TRY(deliver(composed_out_, result, published_));
      RecordingVideo recording;
      recording.data = result.data;
      return deliver(recording_out_, recording, published_);
    });
  }

  gf::Status release() override {
    compositor_.reset();
    return {};
  }
};

class VideoOutputNode final : public Node {
 public:
  using Node::Node;

 private:
  void bind(gf::Binder& ports) override {
    listen<ComposedVideo>(ports, [this](const ComposedVideo& event) -> Result<void> {
      const auto& frame = *event.data;
      LK_STUDIO_ASSIGN(video, to_i420(*frame.program));
      auto& s = shared();
      LK_STUDIO_ASSIGN(result, s.programVideo->write(video));
      if (frame.program == frame.local) {
        LK_STUDIO_TRY(s.localVideo->write(video));
      } else {
        LK_STUDIO_ASSIGN(local, to_i420(*frame.local));
        LK_STUDIO_TRY(s.localVideo->write(local));
      }
      if (result == VideoWriteStatus::backpressure && s.programVideo->stats().attached) {
        std::lock_guard lock(s.mutex);
        ++s.status.dropped;
      }
      return Result<void>::success();
    });
  }

  gf::Status release() override {
    shared().programVideo->close();
    shared().localVideo->close();
    return {};
  }
};

class AudioOutputNode final : public Node {
 public:
  using Node::Node;

 private:
  void bind(gf::Binder& ports) override {
    listen<MixedAudio>(ports, [this](const MixedAudio& event) -> Result<void> {
      const auto& audio = *event.data;
      PcmInputFrame frame;
      frame.capture_time = audio.captured;
      frame.float_samples = audio.local;
      LK_STUDIO_TRY(shared().localAudio->write(frame));
      frame.float_samples = audio.program;
      const auto& trace = shared().traceProgramWrite;
      const auto enter = trace ? Clock::now() : Time{};
      LK_STUDIO_ASSIGN(admission, shared().programAudio->write(frame));
      if (trace) {
        trace(audio.captured, audio.mixed, enter, Clock::now(), admission);
      }
      return Result<void>::success();
    });
  }

  gf::Status release() override {
    shared().programAudio->close();
    shared().localAudio->close();
    return {};
  }
};

class PreviewNode final : public Node {
 public:
  using Node::Node;

 private:
  void bind(gf::Binder& ports) override {
    listen<ComposedVideo>(ports, [this](const ComposedVideo& event) {
      auto& s = shared();
      std::lock_guard lock(s.mutex);
      s.status.preview = event.data->program;
      ++s.status.frames;
      s.status.source_errors = context_.source_errors;
      s.status.source_errors.insert(context_.audio_errors.begin(), context_.audio_errors.end());
      if (!context_.monitor_error.empty())
        s.status.source_errors["monitor"] = context_.monitor_error;
    });
  }
};

class MonitorNode final : public Node {
 public:
  using Node::Node;

 private:
  std::unique_ptr<Monitor> monitor_;
  Time retry_{};

  void bind(gf::Binder& ports) override {
    listen<MixedAudio>(ports, [this](const MixedAudio& event) {
      const auto& audio = *event.data;
      if (!audio.monitor) {
        monitor_.reset();
        std::lock_guard lock(shared().mutex);
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
      std::lock_guard lock(shared().mutex);
      context_.monitor_error = monitor_->error();
    });
  }

  gf::Status stop() override {
    monitor_.reset();
    retry_ = {};
    std::lock_guard lock(shared().mutex);
    context_.monitor_error.clear();
    return {};
  }

  gf::Status release() override {
    return stop();
  }
};

class AudioRecordNode final : public Node {
 public:
  using Node::Node;

 private:
  void bind(gf::Binder& ports) override {
    listen<RecordingAudio>(ports, [this](const RecordingAudio& event) {
      const auto& audio = *event.data;
      std::lock_guard lock(shared().mutex);
      if (shared().recordWanted && context_.recording &&
          audio.captured >= context_.recording_origin) {
        context_.recording->audio(audio.program, audio.captured);
      }
    });
  }
};

class RecorderNode final : public Node {
 public:
  using Node::Node;

 private:
  std::unique_ptr<RecordingSession> session_;

  void publishStatus() {
    if (!session_) {
      return;
    }
    const auto status = session_->status();
    auto& s = shared();
    {
      std::lock_guard lock(s.mutex);
      if (status.first) {
        s.recordWanted = false;
        s.status.recording = false;
        s.status.failure = status.first;
        s.status.error = status.first->message;
        if (status.cleanup) {
          s.status.error += "; cleanup: " + status.cleanup->message;
        }
      }
      if (status.done) {
        context_.recording = nullptr;
        s.status.recording = false;
        s.recordBusy = false;
        s.finalizedRecordEvents = status.admitted;
      }
    }
    if (status.done) {
      session_.reset();
    }
  }

  void control(const TickData& tick) {
    auto& s = shared();
    std::filesystem::path path;
    {
      std::lock_guard lock(s.mutex);
      if (!s.recordWanted && session_) {
        context_.recording = nullptr;
        session_->stop();  // Signal only; never wait for FFmpeg on a Studio tick.
      }
      if (!session_ && s.recordWanted) {
        path = s.recordPath;
        auto name = path8(path);
        if (!name) {
          s.status.failure = name.failure_record();
          s.status.error = name.error().message;
          s.recordWanted = false;
          return;
        }
        s.recordBusy = true;
        s.status.recording_path = std::move(*name);
      } else if (!session_) {
        return;
      }
    }
    if (!session_) {
      if (s.beforeRecordOpen) {
        s.beforeRecordOpen();
      }
      session_ = std::make_unique<RecordingSession>(
          path, s.options, [requested = tick.now] { return std::max(requested, Clock::now()); },
          s.beforeRecordWrite, s.traceEncoderAudio);
      const auto prepared = session_->status();
      if (prepared.mediaOrigin) {
        if (s.traceRecordReady) {
          s.traceRecordReady(tick.now, *prepared.mediaOrigin);
        }
        std::lock_guard lock(s.mutex);
        context_.recording_origin = *prepared.mediaOrigin;
        context_.recording = session_.get();
        s.status.recording = s.recordWanted;
      }
    }
    publishStatus();
  }

  void bind(gf::Binder& ports) override {
    listen<Tick>(ports, [this](const Tick& event) { control(*event.data); });
    listen<RecordingVideo>(ports, [this](const RecordingVideo& event) {
      if (session_) {
        {
          std::lock_guard lock(shared().mutex);
          if (shared().recordWanted && context_.recording &&
              event.data->captured >= context_.recording_origin) {
            session_->video(event.data->program, event.data->captured);
          }
        }
        publishStatus();
      }
    });
  }

  gf::Status stop() override {
    if (session_) {
      {
        std::lock_guard lock(shared().mutex);
        context_.recording = nullptr;
      }
      session_->wait();
      publishStatus();
    }
    std::lock_guard lock(shared().mutex);
    shared().recordWanted = false;
    return {};
  }

  gf::Status release() override {
    return stop();
  }
};

template <class Event, class From, class To>
void link(gf::Wiring& wiring, const From& from, const To& to) {
  wiring.connect(from->out(gf::Out<Event>{Event::port}), to->in(gf::In<Event>{Event::port}));
}

// Both graphs use synchronous delivery. Only the audio owner calls into its graph;
// control and diagnostics run between slots, never concurrently with PCM callbacks.
class AudioLane {
 public:
  explicit AudioLane(Context& context) : context_(context), worker_([this] { loop(); }) {}

  ~AudioLane() {
    call([&] {
      ticking_ = false;
      engine_.reset();
      exit_ = true;
    });
    worker_.join();
  }

  void prepare() {
    call([&] { build(); });
  }

  Result<void> start() {
    call([&] {
      if (failure()) {
        return;
      }
      if (auto status = engine_->start(); !status) {
        fail(graph_error(status.error));
        return;
      }
      next_ = Clock::now();
      ticking_ = true;
      tick();
    });
    if (auto f = failure()) {
      return failed(f);
    }
    return Result<void>::success();
  }

  void stop(bool close) {
    call([&] {
      ticking_ = false;
      if (engine_) {
        auto status = close ? engine_->close() : engine_->stop();
        if (!status) {
          fail(graph_error(status.error));
        }
      }
    });
  }

  Failure failure() {
    std::lock_guard lock(mutex_);
    return failure_;
  }

  void inspect(std::vector<StudioGraph::NodeInfo>* nodes,
               std::vector<StudioGraph::Connection>* edges) {
    call([&] {
      if (!engine_) {
        return;
      }
      if (auto snapshot = engine_->snapshot()) {
        if (nodes) {
          for (const auto& n : snapshot->nodes) {
            nodes->push_back({n.id, n.accepted, published_.at(n.id)});
          }
        }
        if (edges) {
          for (const auto& e : snapshot->edges) {
            edges->push_back({e.from, e.to, e.input});
          }
        }
      }
    });
  }

 private:
  Context& context_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::function<void()> command_;
  bool pending_ = false, exit_ = false, ticking_ = false;
  Failure failure_;
  Time next_;
  std::map<std::string, uint64_t> published_;
  gf::Emitter<AudioTick> writer_;
  std::unique_ptr<gf::Graph> engine_;
  std::thread worker_;

  void fail(Result<void> result) {
    ticking_ = false;
    std::lock_guard lock(mutex_);
    if (!failure_) {
      failure_ = result.failure_record();
    }
  }

  void call(std::function<void()> command) {
    std::unique_lock lock(mutex_);
    command_ = std::move(command);
    pending_ = true;
    wake_.notify_one();
    wake_.wait(lock, [&] { return !pending_; });
  }

  void loop() {
    std::unique_lock lock(mutex_);
    while (!exit_) {
      if (pending_) {
        auto cmd = std::move(command_);
        lock.unlock();
        cmd();
        lock.lock();
        pending_ = false;
        wake_.notify_one();
      } else if (!ticking_) {
        wake_.wait(lock, [&] { return pending_; });
      } else if (!wake_.wait_until(lock, next_, [&] { return pending_; })) {
        lock.unlock();
        tick();
        lock.lock();
      }
    }
  }

  void tick() {
    // More than the input FIFO horizon cannot be recovered with valid captured media.
    if (Clock::now() - next_ > std::chrono::milliseconds(200)) {
      fail(failed("studio/audio-clock: owner missed input buffer horizon",
                  ErrorCode::resource_exhausted));
      return;
    }
    auto result = deliver(writer_, AudioTick{next_}, published_.at("studio.audio-clock"));
    next_ += std::chrono::milliseconds(10);
    if (!result) {
      fail(std::move(result));
    }
  }

  template <class T>
  auto add(const char* id) {
    return engine_->add<T>(id, context_, published_[id]);
  }

  void build() {
    engine_ = std::make_unique<gf::Graph>(gf::GraphOptions{gf::Dispatch::Inline});
    auto clock = engine_->ingress<AudioTick>("studio.audio-clock");
    auto source = add<AudioSourcesNode>("studio.audio-sources");
    auto mixer = add<MixerNode>("studio.mixer");
    auto output = add<AudioOutputNode>("studio.audio-output");
    auto monitor = add<MonitorNode>("studio.monitor");
    auto recorder = add<AudioRecordNode>("studio.audio-recorder");
    for (const auto& status : {clock.status(), source.status(), mixer.status(), output.status(),
                               monitor.status(), recorder.status()}) {
      if (!status) {
        fail(graph_error(status.error));
        return;
      }
    }
    auto wired = engine_->rewire([&](gf::Wiring& w) {
      w.connect(clock->port, source->in(gf::In<AudioTick>{AudioTick::port}));
      link<AudioInput>(w, source, mixer);
      link<MixedAudio>(w, mixer, output);
      link<MixedAudio>(w, mixer, monitor);
      link<RecordingAudio>(w, mixer, recorder);
    });
    if (!wired) {
      fail(graph_error(wired.error));
      return;
    }
    writer_ = clock->writer;
    published_.emplace("studio.audio-clock", 0);
  }
};

}  // namespace

StudioGraphState::StudioGraphState(StudioOptions o) : options(o) {
  if ((o.width != 1280 || o.height != 720) && (o.width != 720 || o.height != 1280) &&
      (o.width != 1920 || o.height != 1080) && (o.width != 1080 || o.height != 1920)) {
    status.failure = make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                  "请选择 720p 或 1080p 横竖屏");
  }
  if (o.fps != 30)
    status.failure =
        make_failure(ErrorDomain::media, ErrorCode::invalid_argument, "首版帧率为 30 fps");
  if (status.failure) {
    status.error = status.failure->message;
    return;
  }
  I420SourceOptions video;
  video.width = o.width;
  video.height = o.height;
  programVideo = std::make_shared<I420VideoSource>(video);
  localVideo = std::make_shared<I420VideoSource>(video);
  PcmSourceOptions audio;
  audio.channels = 2;
  // Capacity is not the startup target. Leave room for a delayed program reader
  // and catch-up writes without increasing the four-block startup prebuffer.
  audio.max_queue_duration = std::chrono::milliseconds(120);
  audio.format = AudioSampleFormat::float32;
  programAudio = detail::PcmSourceWriter::studioOutput(audio);
  localAudio = detail::PcmSourceWriter::studioOutput(audio);
  for (const auto& result : {programVideo->status(), localVideo->status(), programAudio->status(),
                             localAudio->status()}) {
    if (!result) {
      status.failure = result.failure_record();
      status.error = result.error().message;
      break;
    }
  }
}

struct StudioGraph::Impl {
  Context context;
  std::map<std::string, uint64_t> published;
  ClockSource clock;
  AudioLane audio;
  gf::Emitter<Tick> ticks;
  bool running = false;
  // Destroy the graph before the state and counters referenced by its nodes.
  gf::Graph engine{{gf::Dispatch::Inline}};

  template <class T>
  auto add(const char* name) {
    auto result = engine.add<T>(name, context, published[name]);
    if (!result && !context.failure) {
      context.failure = graph_error(result.status().error).failure_record();
    }
    return result;
  }

  explicit Impl(StudioGraphState& shared)
      : context{shared, shared.status.failure}, clock(shared), audio(context) {
    if (context.failure) {
      return;
    }
    auto ingress = engine.ingress<Tick>("studio.clock");
    if (!ingress) {
      context.failure = graph_error(ingress.status().error).failure_record();
      return;
    }
    published.emplace("studio.clock", 0);
    ticks = ingress->writer;
    auto sources = add<SourcesNode>("studio.sources");
    auto compositor = add<CompositeNode>("studio.compositor");
    auto video = add<VideoOutputNode>("studio.video-output");
    auto preview = add<PreviewNode>("studio.preview");
    auto recorder = add<RecorderNode>("studio.recorder");
    if (context.failure) {
      return;
    }
    auto wired = engine.rewire([&](gf::Wiring& w) {
      // Inline fan-out follows connection order: control precedes this tick's media.
      w.connect(ingress->port, recorder->in(gf::In<Tick>{Tick::port}));
      w.connect(ingress->port, sources->in(gf::In<Tick>{Tick::port}), "video");
      link<VideoLayers>(w, sources, compositor);
      link<ComposedVideo>(w, compositor, video);
      link<ComposedVideo>(w, compositor, preview);
      link<RecordingVideo>(w, compositor, recorder);
    });
    if (!wired) {
      context.failure = graph_error(wired.error).failure_record();
    } else {
      audio.prepare();  // Bind both real topologies before start; no capture or playout yet.
      context.failure = audio.failure();
    }
  }
};

StudioGraph::StudioGraph(StudioGraphState& state) : impl_(std::make_unique<Impl>(state)) {}

StudioGraph::~StudioGraph() {
  release();
}

Result<void> StudioGraph::start() {
  if (impl_->context.failure) {
    return failed(impl_->context.failure);
  }
  auto status = impl_->engine.start();
  if (!status) {
    return graph_error(status.error);
  }
  if (!impl_->running) {
    impl_->clock.reset();
    // Establish a requested recording before either media owner submits its first slot.
    auto control = std::make_shared<TickData>();
    control->now = Clock::now();
    auto admitted = deliver(impl_->ticks, Tick{control}, impl_->published["studio.clock"]);
    if (!admitted) {
      return admitted;
    }
    auto audio = impl_->audio.start();
    if (!audio) {
      impl_->context.failure = audio.failure_record();
      stop();
      return audio;
    }
    impl_->running = true;
  }
  return Result<void>::success();
}

Result<void> StudioGraph::step(Time now) {
  if (!impl_->running) {
    return failed("studio/step: graph is not running", ErrorCode::invalid_state);
  }
  if (impl_->context.failure) {
    return failed(impl_->context.failure);
  }
  if (auto f = impl_->audio.failure()) {
    return failed(f);
  }
  auto result =
      deliver(impl_->ticks, impl_->clock.tick(now), impl_->published["studio.clock"], "video");
  if (!result) {
    impl_->context.failure = result.failure_record();
  }
  return result;
}

void StudioGraph::stop() {
  impl_->running = false;
  impl_->audio.stop(false);
  (void)impl_->engine.stop();
}

void StudioGraph::release() {
  impl_->running = false;
  impl_->audio.stop(true);
  (void)impl_->engine.close();
}

Result<gf::GraphState> StudioGraph::state() const {
  auto view = impl_->engine.snapshot();
  if (!view) {
    return failed(graph_error(view.status().error).failure_record());
  }
  return Result<gf::GraphState>::success(view->state);
}

std::vector<StudioGraph::NodeInfo> StudioGraph::nodes() const {
  std::vector<NodeInfo> result;
  if (auto view = impl_->engine.snapshot()) {
    for (const auto& node : view->nodes) {
      result.push_back({node.id, node.accepted, impl_->published.at(node.id)});
    }
  }
  impl_->audio.inspect(&result, nullptr);
  return result;
}

std::vector<StudioGraph::Connection> StudioGraph::connections() const {
  std::vector<Connection> result;
  if (auto view = impl_->engine.snapshot()) {
    for (const auto& edge : view->edges) {
      result.push_back({edge.from, edge.to, edge.input});
    }
  }
  impl_->audio.inspect(nullptr, &result);
  return result;
}
}  // namespace livekit::sdk::studio_detail
